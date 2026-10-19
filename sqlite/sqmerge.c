/*
 * sqmerge.c - the merge of segments for the SQLite table: the engine's own (src/bm25/sift_runs.c, the merge of the PostgreSQL host
 * too) turns the live part of every input into a sorted run and merges the runs into one segment, in bounded memory. The output
 * is published and the inputs retired in the same SQLite transaction by two manifest updates, so a rollback or a crash at any
 * point leaves the old segments in place: there is nothing to resume, a repeated merge starts over from the manifest. Which
 * segments to merge is the shared policy (src/bm25/sift_merge.c); the live documents and terms of a segment, for the integrity
 * check and the facets, are read here.
 */
#include "sqapi.h"

#include "sift_dict.h"
#include "sift_runs.h"

#define TIER_BASE	(1024 * 1024)		/* segments below this size are one tier */

/* ---------- live documents ---------- */

typedef void (*DocFn) (void *arg, SiftKey key, uint16 dl);

static void
walk_live_docs(SiftSegment *seg, DocFn fn, void *arg)
{
	SiftDocChunk dc;
	uint32		sb = 0;

	while (sift_seg_docs_next(seg, sb, &dc))
	{
		for (int p = 0; p < SIFT_SB_PAGES; p++)
		{
			uint64		present[SIFT_OFFMASK_WORDS],
						live[SIFT_OFFMASK_WORDS];
			const uint16 *dl;

			if (!(dc.pagemask[p / 64] >> (p % 64) & 1))
				continue;
			sift_seg_page(seg, &dc, p, present, &dl);
			sift_seg_page_live(seg, &dc, p, live);
			for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
				for (uint64 m = live[w]; m; m &= m - 1)
				{
					int			o = w * 64 + pg_rightmost_one_pos64(m);

					fn(arg, ((SiftKey) (dc.sb * SIFT_SB_PAGES + p) << 16) | (o + 1), dl[o]);
				}
		}
		sb = dc.sb + 1;
	}
}

void
sq_seg_live_docs(SqStore *st, const SiftSegEntry *e, void (*fn) (void *arg, SiftKey key, uint16 dl), void *arg)
{
	SiftSegment *seg = sift_seg_open(sq_store_rel(st), e, CurrentMemoryContext);

	walk_live_docs(seg, fn, arg);
	sift_seg_close(seg);
}

/* ---------- the live postings of every term, for readers other than the merge: the engine's own dump ---------- */

typedef struct TermWalk
{
	void		(*fn) (void *arg, const char *term, int len, const SiftKey *keys, uint32 n);
	void	   *arg;
	MemoryContext caller;				/* fn allocates in the caller's context, the walk's goes away */
} TermWalk;

static void
no_docs(void *arg, const SiftKey *keys, const uint16 *dl, int n)
{
}

static bool
walk_term(void *arg, const char *term, int len, const SiftTermInfo *info, const SiftKey *keys, const uint8 *tf,
		  const uint16 *dl, const uint8 *pos, const uint32 *poslen)
{
	TermWalk   *w = arg;
	MemoryContext old = MemoryContextSwitchTo(w->caller);

	w->fn(w->arg, term, len, keys, info->df);
	MemoryContextSwitchTo(old);
	return true;
}

void
sq_seg_walk_terms(SqStore *st, const SiftSegEntry *e,
				  void (*fn) (void *arg, const char *term, int len, const SiftKey *keys, uint32 n), void *arg)
{
	const SqOptions *opt = sq_store_options(st);
	SiftScoreRef ref = {(float4) opt->base.k1, (float4) opt->base.b, 0};
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift walk", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	TermWalk	w = {.fn = fn,.arg = arg,.caller = old};
	SiftSegment *seg = sift_seg_open(sq_store_rel(st), e, cx);

	sift_seg_dump(seg, &ref, no_docs, walk_term, &w);
	sift_seg_close(seg);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* ---------- merge ---------- */

void
sq_merge(SqStore *st, const uint64 *gens, int n)
{
	const SqOptions *opt = sq_store_options(st);
	SiftScoreRef ref = {(float4) opt->base.k1, (float4) opt->base.b, 0};
	const SiftDirSnapshot *dir = sq_store_dir(st);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftRun   **runs = palloc(n * sizeof(SiftRun *));
	int			nruns = 0;

	for (int g = 0; g < n; g++)
	{
		SiftSegment *seg;
		SiftRun    *run;
		int			i = 0;

		while (i < dir->nsegs && dir->segs[i].gen != gens[g])
			i++;
		if (i == dir->nsegs)
			elog(ERROR, "sift: segment %llu is not in the directory", (unsigned long long) gens[g]);
		seg = sift_seg_open(sq_store_rel(st), &dir->segs[i], cx);
		run = sift_run_from_segment(seg, &ref, cx);
		sift_seg_close(seg);
		if (run)
			runs[nruns++] = run;
	}
	if (nruns > 0)
	{
		SiftWriteTarget *wt = sift_wt_begin(sq_store_rel(st), SIFT_WRITE_SHARED);
		SiftSegEntry entry;

		sift_runs_write(runs, nruns, wt, sq_store_new_gen(st), &ref, &entry);
		entry.reserved = sq_wt_first(wt);
		sift_wt_finish(wt);
		sq_store_publish(st, &entry);
	}
	for (int g = 0; g < n; g++)
		sq_store_retire(st, gens[g]);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* ---------- policy: sift_merge.c, with the tiers starting at 1 MiB ---------- */

int
sq_merge_pick(SqStore *st, bool force, int fanin, uint64 *sel)
{
	SiftMergePolicy pol = {TIER_BASE, fanin};
	SiftSegEntry in[SIFT_MAX_SEGMENTS];
	int			n = sift_merge_pick(sq_store_rel(st), &pol, sq_store_dir(st), force, in);

	for (int i = 0; i < n; i++)
		sel[i] = in[i].gen;
	return n;
}
