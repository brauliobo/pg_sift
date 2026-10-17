/*
 * sqmerge.c - tiered, streaming merge of segments. A merge reads up to a few segments in lockstep and writes one:
 * the documents first (the live ones of every input, merged by key), then the terms in dictionary order, one at a
 * time (the postings of the term in every input, merged by key). Memory is the live documents of the inputs (key and
 * dl, 10 bytes each) plus one term's postings and positions, never the whole inverted index. The output is published
 * and the inputs retired in the same SQLite transaction by two manifest updates, so a rollback or a crash at any point
 * leaves the old segments in place: there is nothing to resume, a repeated merge starts over from the manifest.
 */
#include "sqapi.h"

#include "sift_dict.h"
#include "sift_kernels.h"

#define BATCH		1024
#define TERM_BATCH	256
#define TIER_BASE	(1024 * 1024)		/* segments below this size are one tier */

typedef struct TermRec
{
	char	   *term;
	int			len;
	SiftTermInfo info;
} TermRec;

typedef struct Src
{
	SiftSegment *seg;
	/* live documents, ascending */
	SiftKey    *dkey;
	uint16	   *ddl;
	uint64		nd,
				dcap;
	/* dictionary, TERM_BATCH terms at a time */
	MemoryContext bcx;
	TermRec		rec[TERM_BATCH];
	int			nrec,
				pos;
	bool		more;
	char		last[SIFT_MAX_TERM_BYTES + 1];
	int			lastlen;
	/* the current term's live postings */
	MemoryContext tcx;
	uint32		n,
				idx;
	SiftKey    *k;
	uint8	   *tf;
	uint16	   *dl;
	uint32	   *poff,
			   *plen;
	const uint8 *recs;					/* position records of the term, NULL when it has none */
	bool		haspos;
} Src;

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

static void
add_doc(void *arg, SiftKey key, uint16 dl)
{
	Src		   *s = arg;

	if (s->nd == s->dcap)
	{
		s->dcap = s->dcap ? s->dcap * 2 : 4096;
		s->dkey = s->dkey ? repalloc_huge(s->dkey, s->dcap * sizeof(SiftKey)) : palloc(s->dcap * sizeof(SiftKey));
		s->ddl = s->ddl ? repalloc_huge(s->ddl, s->dcap * sizeof(uint16)) : palloc(s->dcap * sizeof(uint16));
	}
	s->dkey[s->nd] = key;
	s->ddl[s->nd++] = dl;
}

/* ---------- dictionary batches ---------- */

static bool
batch_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Src		   *s = arg;
	TermRec    *r = &s->rec[s->nrec++];

	r->term = memcpy(palloc(len), term, len);
	r->len = len;
	r->info = *info;
	if ((info->flags & SIFT_TERM_INLINE) && info->inline_pos)
		r->info.inline_pos = memcpy(palloc(Max(info->pos_len, 1)), info->inline_pos, info->pos_len);
	return s->nrec < TERM_BATCH;
}

static void
fill_batch(Src *s)
{
	MemoryContext old;

	MemoryContextReset(s->bcx);
	old = MemoryContextSwitchTo(s->bcx);
	s->nrec = s->pos = 0;
	sift_dict_scan(s->seg->dict, s->lastlen ? s->last : NULL, s->lastlen, NULL, 0, batch_term, s);
	MemoryContextSwitchTo(old);
	s->more = s->nrec == TERM_BATCH;
	if (s->nrec > 0)
	{
		TermRec    *r = &s->rec[s->nrec - 1];

		memcpy(s->last, r->term, r->len);
		s->last[r->len] = '\0';			/* the smallest term after it */
		s->lastlen = r->len + 1;
	}
}

static inline TermRec *
current(Src *s)
{
	if (s->pos >= s->nrec && s->more)
		fill_batch(s);
	return s->pos < s->nrec ? &s->rec[s->pos] : NULL;
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

/* ---------- one term of one segment ---------- */

/* The position of key among the live documents, searching on from *at; false when it is dead */
static bool
find_live(const Src *s, SiftKey key, uint64 *at)
{
	uint64		lo = *at,
				hi = s->nd,
				step = 1;

	while (lo + step < hi && s->dkey[lo + step] <= key)
		step *= 2;
	hi = Min(hi, lo + step + 1);
	while (lo < hi)
	{
		uint64		mid = (lo + hi) / 2;

		if (s->dkey[mid] < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	*at = lo;
	return lo < s->nd && s->dkey[lo] == key;
}

/* The live postings of the term with their tf, dl and position records, in key order */
static void
decode_term(Src *s, const TermRec *r)
{
	const SiftTermInfo *info = &r->info;
	MemoryContext old;
	SiftPostingCursor *c;
	sift_tid   *tids;
	uint8	   *gtf,
			   *rec = NULL;
	uint32		n = 0,
				m = 0,
				reclen = 0;
	uint32	   *poff,
			   *plen;
	uint64		at = 0;

	MemoryContextReset(s->tcx);
	old = MemoryContextSwitchTo(s->tcx);
	tids = palloc(SIFT_GRP_MAXTIDS * sizeof(sift_tid));
	gtf = palloc(SIFT_GRP_MAXTIDS + SIFT_KERN_PAD);
	s->k = palloc(info->df * sizeof(SiftKey));
	s->tf = palloc(info->df);
	s->dl = palloc(info->df * sizeof(uint16));
	poff = palloc(info->df * sizeof(uint32));
	plen = palloc(info->df * sizeof(uint32));
	s->poff = poff;
	s->plen = plen;
	c = sift_post_open(&s->seg->sr, info, s->tcx);
	for (const sift_grp *g = sift_next(sift_post_cursor(c)); g; g = sift_next(sift_post_cursor(c)))
	{
		uint32		k = sift_grp_tids(g, NULL, tids);

		if (n + k > info->df)
			elog(ERROR, "sift: term decodes past its df %u", info->df);
		sift_post_group_tf(c, gtf);
		for (uint32 j = 0; j < k; j++)
		{
			s->k[n + j] = tids[j];
			s->tf[n + j] = gtf[j];
		}
		n += k;
	}
	if (n != info->df)
		elog(ERROR, "sift: term has %u postings, df %u", n, info->df);
	s->haspos = (info->flags & SIFT_TERM_POS) != 0;
	if (s->haspos)
	{
		const uint32 *ck;
		const uint8 *src,
				   *p;

		sift_post_pos_stream(c, &ck, &src, &reclen);
		rec = memcpy(palloc(Max(reclen, 1)), src, reclen);
		p = rec;
		for (uint32 j = 0; j < n; j++)
		{
			uint32		cnt,
						last;
			const uint8 *q = sift_pos_skip(p, rec + reclen, &cnt, &last);

			poff[j] = p - rec;
			plen[j] = q - p;
			p = q;
		}
	}
	s->recs = rec;
	for (uint32 j = 0; j < n; j++)
		if (find_live(s, s->k[j], &at))
		{
			s->k[m] = s->k[j];
			s->tf[m] = s->tf[j];
			s->dl[m] = s->ddl[at];
			poff[m] = poff[j];
			plen[m] = plen[j];
			m++;
		}
	s->n = m;
	s->idx = 0;
	MemoryContextSwitchTo(old);
}

/* ---------- output ---------- */

typedef struct Out
{
	SiftPostingWriter *pw;
	int			n;
	SiftKey		keys[BATCH];
	uint8		tf[BATCH];
	uint16		dl[BATCH];
	uint32		plen[BATCH];
	uint8	   *pos;
	Size		poslen,
				poscap;
	bool		haspos;
} Out;

static void
out_flush(Out *o)
{
	if (o->n)
		sift_postw_add(o->pw, o->keys, o->tf, o->dl, o->n, o->haspos ? o->pos : NULL, o->plen);
	o->n = 0;
	o->poslen = 0;
}

static void
out_add(Out *o, const Src *s)
{
	uint32		i = s->idx;

	o->keys[o->n] = s->k[i];
	o->tf[o->n] = Min(s->tf[i], SIFT_MAX_TF);
	o->dl[o->n] = s->dl[i];
	if (o->haspos)
	{
		if (o->poslen + s->plen[i] > o->poscap)
		{
			o->poscap = Max(o->poslen + s->plen[i], Max(8192, o->poscap * 2));
			o->pos = o->pos ? repalloc_huge(o->pos, o->poscap) : palloc(o->poscap);
		}
		memcpy(o->pos + o->poslen, s->recs + s->poff[i], s->plen[i]);
		o->plen[o->n] = s->plen[i];
		o->poslen += s->plen[i];
	}
	if (++o->n == BATCH)
		out_flush(o);
}

static int
term_cmp(const TermRec *a, const TermRec *b)
{
	int			c = memcmp(a->term, b->term, Min(a->len, b->len));

	return c ? c : a->len - b->len;
}

/* The inputs' postings of one term, merged by key (their documents are disjoint) */
static void
merge_term(SiftSegmentWriter *w, Out *o, Src **in, int nin)
{
	const TermRec *r = current(in[0]);
	bool		haspos = true;
	bool		any = false;

	for (int i = 0; i < nin; i++)
		if (in[i]->n > 0)
		{
			any = true;
			haspos &= in[i]->haspos;
		}
	if (!any)
		return;
	o->pw = sift_segw_begin_term(w, r->term, r->len);
	o->haspos = haspos;
	for (;;)
	{
		Src		   *best = NULL;

		for (int i = 0; i < nin; i++)
			if (in[i]->idx < in[i]->n && (best == NULL || in[i]->k[in[i]->idx] < best->k[best->idx]))
				best = in[i];
		if (best == NULL)
			break;
		out_add(o, best);
		best->idx++;
	}
	out_flush(o);
	sift_segw_end_term(w);
}

static void
write_docs(SiftSegmentWriter *w, Src *src, int n)
{
	SiftKey		keys[BATCH];
	uint16		dl[BATCH];
	uint64		at[SIFT_MAX_SEGMENTS] = {0};
	int			k = 0;

	for (;;)
	{
		int			best = -1;

		for (int i = 0; i < n; i++)
			if (at[i] < src[i].nd && (best < 0 || src[i].dkey[at[i]] < src[best].dkey[at[best]]))
				best = i;
		if (best < 0)
			break;
		keys[k] = src[best].dkey[at[best]];
		dl[k++] = src[best].ddl[at[best]++];
		if (k == BATCH)
		{
			sift_segw_add_docs(w, keys, dl, k);
			k = 0;
		}
	}
	if (k)
		sift_segw_add_docs(w, keys, dl, k);
}

static void
write_terms(SiftSegmentWriter *w, Src *src, int n)
{
	Out		   *o = palloc0(sizeof(Out));
	Src		  **in = palloc(n * sizeof(Src *));

	for (;;)
	{
		const TermRec *min = NULL;
		int			nin = 0;

		for (int i = 0; i < n; i++)
		{
			const TermRec *r = current(&src[i]);

			if (r && (min == NULL || term_cmp(r, min) < 0))
				min = r;
		}
		if (min == NULL)
			break;
		for (int i = 0; i < n; i++)
		{
			const TermRec *r = current(&src[i]);

			if (r && term_cmp(r, min) == 0)
			{
				decode_term(&src[i], r);
				in[nin++] = &src[i];
			}
		}
		merge_term(w, o, in, nin);
		for (int i = 0; i < nin; i++)
			in[i]->pos++;
	}
}

void
sq_merge(SqStore *st, const uint64 *gens, int n, bool positions)
{
	const SqOptions *opt = sq_store_options(st);
	SiftScoreRef ref = {(float4) opt->base.k1, (float4) opt->base.b, 0};
	const SiftDirSnapshot *dir = sq_store_dir(st);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	Src		   *src = palloc0(n * sizeof(Src));
	uint64		ndocs = 0;

	for (int g = 0; g < n; g++)
	{
		Src		   *s = &src[g];
		int			i = 0;

		while (i < dir->nsegs && dir->segs[i].gen != gens[g])
			i++;
		if (i == dir->nsegs)
			elog(ERROR, "sift: segment %llu is not in the directory", (unsigned long long) gens[g]);
		s->seg = sift_seg_open(sq_store_rel(st), &dir->segs[i], cx);
		s->bcx = AllocSetContextCreate(cx, "sift merge terms", ALLOCSET_DEFAULT_SIZES);
		s->tcx = AllocSetContextCreate(cx, "sift merge term", ALLOCSET_DEFAULT_SIZES);
		s->more = true;
		walk_live_docs(s->seg, add_doc, s);
		ndocs += s->nd;
	}
	if (ndocs > 0)
	{
		SiftWriteTarget *wt = sift_wt_begin(sq_store_rel(st), SIFT_WRITE_SHARED);
		SiftSegmentWriter *w = sift_segw_begin(wt, sq_store_new_gen(st), &ref, cx, positions);
		SiftSegEntry entry;

		write_docs(w, src, n);
		write_terms(w, src, n);
		sift_segw_finish(w, &entry);
		entry.reserved = sq_wt_first(wt);
		sift_wt_finish(wt);
		for (int g = 0; g < n; g++)
			sift_seg_close(src[g].seg);
		sq_store_publish(st, &entry);
	}
	else
		for (int g = 0; g < n; g++)
			sift_seg_close(src[g].seg);
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
