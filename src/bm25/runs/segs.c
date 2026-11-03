/*
 * bm25/runs/segs.c - the cuts and the range writes read from segments
 * a run made from a segment
 * the dump of a segment into a run
 */
#include "core/host.h"
#include "bm25/runs/runs_priv.h"

/*
 * bm25/runs/runs.c - the sorted runs and their merge into segments (bm25/runs.h). Portable: a run is a
 * BufFile, the merge a binary heap over their readers, the output the segment writer.
 */

#define ULL(x)			((unsigned long long) (x))

/*
 * The input segments of a merge read directly: a reader of each is a RunReader with a cursor in place of a run file, so that the merge of
 * docs and terms is the one of runs. The segments have a live doc each (sift_seg_live_stats).
 */
static RunReader *
open_seg_readers(SiftSegment **segs, int nsegs, Size budget, bool terms, const SiftTermCut *lo, bool *positions)
{
	RunReader  *rd = palloc0(nsegs * sizeof(RunReader));

	*positions = true;
	for (int i = 0; i < nsegs; i++)
	{
		rd[i].idx = i;
		rd[i].sc = sift_seg_cursor(segs[i], terms ? Max(budget / 2 / nsegs, 1024 * 1024) : 0, lo ? lo->term : NULL, lo ? lo->len : 0);
		rd[i].haspos = (segs[i]->entry.flags & SIFT_SEG_POSITIONS) != 0;
		*positions &= rd[i].haspos;
		if (terms)
			runs_reader_next_term(&rd[i]);
	}
	return rd;
}

static void
close_seg_readers(RunReader *rd, int nsegs)
{
	for (int i = 0; i < nsegs; i++)
		sift_seg_cursor_close(rd[i].sc);
}

/* A term of a segment costs about this many bytes of its postings */
#define TERM_COST		32

#define SEG_SAMPLES		256

typedef struct SegSamples
{
	RunSample  *all;
	int			n;
	int			cap;
	uint64		terms;			/* from the last sample to the next */
} SegSamples;

static void
seg_sample(void *arg, const char *term, int len, uint64 post_off, uint64 nterms)
{
	SegSamples *ss = arg;
	char	   *copy = palloc(len);

	if (ss->n > 0)
		ss->all[ss->n - 1].vol = post_off - ss->all[ss->n - 1].off + ss->terms * TERM_COST;
	if (ss->n == ss->cap)
	{
		ss->cap = Max(16, ss->cap * 2);
		ss->all = ss->all ? repalloc(ss->all, ss->cap * sizeof(RunSample)) : palloc(ss->cap * sizeof(RunSample));
	}
	memcpy(copy, term, len);
	ss->all[ss->n++] = (RunSample) {.off = post_off,.len = len,.term = copy};
	ss->terms = nterms;
}

int
sift_segs_cuts(SiftSegment **segs, int nsegs, uint64 min_bytes, int target, SiftTermCut *cuts)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift segment cuts", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SegSamples	ss = {0};
	RunSample  *all = NULL;
	int			nall = 0,
				ncuts;

	for (int i = 0; i < nsegs; i++)
	{
		const SiftSectionRef *post = &segs[i]->hdr->sections[SIFT_SEC_POSTINGS];
		int			first = nall;

		ss.n = ss.cap = 0;
		ss.all = NULL;
		sift_dict_samples(segs[i]->dict, SEG_SAMPLES, seg_sample, &ss);
		if (ss.n > 0)
			ss.all[ss.n - 1].vol = Max(post->off + post->len, ss.all[ss.n - 1].off) - ss.all[ss.n - 1].off + ss.terms * TERM_COST;
		all = all ? repalloc(all, (nall + ss.n) * sizeof(RunSample)) : palloc((nall + ss.n) * sizeof(RunSample));
		memcpy(all + first, ss.all, ss.n * sizeof(RunSample));
		nall += ss.n;
	}
	ncuts = runs_choose_cuts(all, nall, min_bytes, target, cuts);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return ncuts;
}

SiftSegmentWriter *
sift_segs_write_docs(SiftSegment **segs, int nsegs, Size budget, SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref, MemoryContext mcxt)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift segment docs", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	MergeOut	mo = {0};
	bool		positions;
	RunReader  *rd = open_seg_readers(segs, nsegs, budget, false, NULL, &positions);

	mo.sw = runs_segw_begin(wt, gen, ref, mcxt, positions);
	runs_merge_docs(&mo, rd, nsegs);
	close_seg_readers(rd, nsegs);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return mo.sw;
}

void
sift_segs_write_range(SiftSegment **segs, int nsegs, Size budget, const SiftTermCut *lo, const SiftTermCut *hi, SiftSegmentWriter *rw)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift segment range", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	MergeOut	mo = {.sw = rw};
	bool		positions;
	RunReader  *rd = open_seg_readers(segs, nsegs, budget, true, lo, &positions);

	runs_merge_terms(rd, nsegs, positions, &mo, hi);
	close_seg_readers(rd, nsegs);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

typedef struct DumpState
{
	RunIO		io;
	SiftKey		prev;
	uint64		ndocs;
	SiftRun    *run;
} DumpState;

static void
dump_docs(void *arg, const SiftKey *keys, const uint16 *dl, int n)
{
	DumpState  *ds = arg;

	for (int i = 0; i < n; ds->prev = keys[i++])
	{
		io_varint(&ds->io, keys[i] - ds->prev);
		io_varint(&ds->io, dl[i]);
	}
	ds->ndocs -= n;
}

static void
dump_term(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	DumpState  *ds = arg;
	bool		haspos = (info->flags & SIFT_TERM_POS) != 0;

	io_varint(&ds->io, len);
	io_put(&ds->io, term, len);
	io_varint(&ds->io, info->df);
	io_varint(&ds->io, haspos);
	ds->prev = 0;
	runs_run_note_term(ds->run, len, info->df, haspos ? info->pos_len : 0);
}

static void
dump_postings(void *arg, const SiftKey *keys, const uint8 *tf, const uint16 *dl, const uint8 *pos, const uint32 *poslen,
			  int n)
{
	DumpState  *ds = arg;

	for (int j = 0; j < n; ds->prev = keys[j++])
	{
		io_varint(&ds->io, (keys[j] - ds->prev) << 1 | (tf[j] > 1));
		if (tf[j] > 1)
			io_put(&ds->io, &tf[j], 1);
		io_varint(&ds->io, dl[j]);
		if (pos)
		{
			io_put(&ds->io, pos, poslen[j]);
			pos += poslen[j];
		}
	}
}

SiftRun *
sift_run_from_segment(SiftSegment *seg, const SiftScoreRef *ref, MemoryContext mcxt)
{
	uint64		ndocs = sift_seg_live_docs(seg);
	SiftRun    *run;
	DumpState	ds = {.ndocs = ndocs};
	MemoryContext old;

	if (ndocs == 0)
		return NULL;
	old = MemoryContextSwitchTo(mcxt);
	run = palloc0(sizeof(SiftRun));
	run->file = ds.io.file = BufFileCreateTemp(false);
	ds.run = run;
	ds.io.buf = palloc(IO_BUF);
	run->ndocs = ndocs;
	io_varint(&ds.io, ndocs);
	io_varint(&ds.io, (seg->entry.flags & SIFT_SEG_POSITIONS) != 0);
	sift_seg_stream(seg, (Size) maintenance_work_mem * 1024 / 2, &(SiftSegSink) {dump_docs, dump_term, dump_postings}, &ds);
	SIFT_INVARIANT(ds.ndocs == 0, "sift: segment %llu lists %llu live docs but streamed another number", ULL(seg->entry.gen), ULL(ndocs));
	io_varint(&ds.io, 0);
	io_flush(&ds.io);
	pfree(ds.io.buf);
	MemoryContextSwitchTo(old);
	return run;
}
