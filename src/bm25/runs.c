/*
 * bm25/runs.c - the sorted runs and their merge into segments (bm25/runs.h). Portable: a run is a
 * BufFile, the merge a binary heap over their readers, the output the segment writer.
 */
#include "sift_host.h"

#include "sift_cmp.h"
#include "bm25/runs.h"
#include "bm25/runs_int.h"

#define ULL(x)			((unsigned long long) (x))

#ifdef SIFT_TESTING
int			sift_test_run_docs = 0;
int			sift_test_fanin = 0;
int			sift_test_segment_postings = 0;
int			sift_test_pool_blocks = 0;
void		(*sift_test_merge_term) (uint32 nterms, uint64 df) = NULL;
#endif

/* The term of a run: len bytes, df postings and posbytes bytes of position records */
static void
run_note_term(SiftRun *run, int len, uint64 df, uint64 posbytes)
{
	run->maxdf = Max(run->maxdf, df);
	run->maxblob = Max(run->maxblob, df * BLOB_POSTING_BYTES + posbytes);
	run->dict += len + DICT_ENTRY_BYTES;
}

/* Postings of the largest term of a segment that fit its memory: the posting writer takes about 10 bytes of each */
uint64
sift_seg_postings(Size budget)
{
	return TEST_KNOB(sift_test_segment_postings, budget / 10);
}

const char *
sift_run_file(int worker, int run)
{
	static char name[32];

	snprintf(name, sizeof(name), "sift-run-%d-%d", worker, run);
	return name;
}

/* An empty run with its file open for writing (a file of the fileset when there is one) */
SiftRun *
run_new(MemoryContext mcxt, SharedFileSet *fileset, int worker, int id)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftRun    *run = palloc0(sizeof(SiftRun));

	run->fileset = fileset;
	run->worker = worker;
	run->id = id;
	run->file = fileset ? BufFileCreateFileSet(&fileset->fs, sift_run_file(worker, id)) : BufFileCreateTemp(false);
	MemoryContextSwitchTo(old);
	return run;
}

/* The file of a run another process wrote: opened when the run is merged, not while it waits */
static BufFile *
run_open(SiftRun *run)
{
	if (run->file == NULL)
		run->file = BufFileOpenFileSet(&run->fileset->fs, sift_run_file(run->worker, run->id), O_RDONLY, false);
	return run->file;
}

/* Closes a merged run; a fileset's file goes too, as the leader or the worker has no more use for it */
static void
run_close(SiftRun *run)
{
	BufFileClose(run->file);
	if (run->fileset)
		BufFileDeleteFileSet(&run->fileset->fs, sift_run_file(run->worker, run->id), false);
}

/* Runs one merge reads at a time: a reader each (the smallest has two blocks), within half the budget */
int
sift_merge_fanin(Size budget)
{
	return Max(2, TEST_KNOB(sift_test_fanin, Min(MAX_MERGE_FANIN, Max(8, budget / 2 / (2 * BLCKSZ)))));
}

/* Read buffer of each of nruns readers: what half the budget leaves after their file buffers, at most IO_BUF */
static int
reader_buf(Size budget, int nruns)
{
	return Min(IO_BUF, Max(BLCKSZ, (int) Min(budget / 2 / nruns, INT_MAX) - BLCKSZ));
}

/* Merge input: one run positioned on a term */
typedef struct RunReader
{
	RunIO		io;
	int			idx;
	char		term[SIFT_MAX_TERM_BYTES];
	int			len;			/* 0 at end */
	uint64		df;
	bool		haspos;			/* the run holds position records */
	bool		thaspos;		/* ... for the current term */
	/* docs */
	uint64		dleft;
	SiftKey		dkey;
	uint16		ddl;
	/* postings of the current term */
	uint64		pleft;
	SiftKey		pkey;
	uint8		ptf;
	uint16		pdl;
	uint8	   *prec;			/* position record of the current posting */
	uint32		prlen;
	Size		prcap;
} RunReader;

/* ---------- runs ---------- */

void
io_flush(RunIO *io)
{
	BufFileWrite(io->file, io->buf, io->len);
	io->len = 0;
}


static inline uint8
io_byte(RunIO *io)
{
	if (io->pos == io->len)
	{
		io->len = BufFileRead(io->file, io->buf, io->cap);
		io->pos = 0;
		if (io->len == 0)
			elog(ERROR, "sift: unexpected end of a build run");
	}
	return io->buf[io->pos++];
}

static inline uint64
io_get_varint(RunIO *io)
{
	uint64		x = 0;

	for (int s = 0;; s += 7)
	{
		uint8		c = io_byte(io);

		x |= (uint64) (c & 0x7f) << s;
		if (!(c & 0x80))
			return x;
	}
}


static void
reader_next_term(RunReader *rd)
{
	rd->len = io_get_varint(&rd->io);
	if (rd->len == 0)
		return;
	if (rd->len > SIFT_MAX_TERM_BYTES)
		elog(ERROR, "sift: corrupt build run (term of %d bytes)", rd->len);
	for (int i = 0; i < rd->len; i++)
		rd->term[i] = io_byte(&rd->io);
	rd->df = io_get_varint(&rd->io);
	rd->thaspos = io_get_varint(&rd->io);
	if (rd->thaspos && !rd->haspos)
		elog(ERROR, "sift: corrupt build run (positions in a run without)");
}

static int
reader_cmp(Datum a, Datum b, void *arg)
{
	const RunReader *x = (const RunReader *) DatumGetPointer(a),
			   *y = (const RunReader *) DatumGetPointer(b);
	int			c = sift_term_cmp(x->term, x->len, y->term, y->len);

	/* binaryheap keeps the largest on top */
	return c ? -c : y->idx - x->idx;
}

static int
doc_heap_cmp(Datum a, Datum b, void *arg)
{
	const RunReader *x = (const RunReader *) DatumGetPointer(a),
			   *y = (const RunReader *) DatumGetPointer(b);

	return pg_cmp_u64(y->dkey, x->dkey);
}

/* Moves to the next doc of the run; false when it has none */
static bool
reader_next_doc(RunReader *rd)
{
	if (rd->dleft == 0)
		return false;
	rd->dkey += io_get_varint(&rd->io);
	rd->ddl = io_get_varint(&rd->io);
	rd->dleft--;
	return true;
}

static void batch_to_run(Batch *bt);

void
batch_flush(Batch *bt)
{
	if (bt->n && bt->run)
		batch_to_run(bt);
	else if (bt->n)
		sift_postw_add(bt->pw, bt->keys, bt->tf, bt->dl, bt->n, bt->haspos ? bt->pos : NULL, bt->plen);
	bt->n = 0;
	bt->termpos += bt->poslen;
	bt->poslen = 0;
}

/* rec / len: the posting's position record, ignored unless the term has positions */
void
batch_put(Batch *bt, SiftKey key, uint8 tf, uint16 dl, const uint8 *rec, uint32 len)
{
	bt->keys[bt->n] = key;
	bt->tf[bt->n] = tf;
	bt->dl[bt->n] = dl;
	if (bt->haspos)
	{
		if (bt->poslen + len > bt->poscap)
		{
			bt->poscap = Max(bt->poslen + len, Max(8192, bt->poscap * 2));
			bt->pos = bt->pos ? repalloc_huge(bt->pos, bt->poscap) : MemoryContextAllocHuge(CurrentMemoryContext, bt->poscap);
		}
		memcpy(bt->pos + bt->poslen, rec, len);
		bt->plen[bt->n] = len;
		bt->poslen += len;
	}
	if (++bt->n == BATCH)
		batch_flush(bt);
}

/* The postings of the batch in the run format (see spill_to) */
static void
batch_to_run(Batch *bt)
{
	Size		off = 0;

	for (int k = 0; k < bt->n; bt->prev = bt->keys[k++])
	{
		io_varint(bt->run, (bt->keys[k] - bt->prev) << 1 | (bt->tf[k] > 1));
		if (bt->tf[k] > 1)
			io_put(bt->run, &bt->tf[k], 1);
		io_varint(bt->run, bt->dl[k]);
		if (bt->haspos)
		{
			io_put(bt->run, bt->pos + off, bt->plen[k]);
			off += bt->plen[k];
		}
	}
}

/* Where a merge writes: a segment, or a run (the format of spill_to) that a later merge reads */
typedef struct MergeOut
{
	SiftSegmentWriter *sw;
	SiftRun    *run;			/* the run that is written, when there is no sw */
	RunIO		io;
	SiftKey		prev;
} MergeOut;

static void
out_docs(MergeOut *mo, const SiftKey *keys, const uint16 *dl, int n)
{
	if (mo->sw)
		sift_segw_add_docs(mo->sw, keys, dl, n);
	else
		for (int i = 0; i < n; mo->prev = keys[i++])
		{
			io_varint(&mo->io, keys[i] - mo->prev);
			io_varint(&mo->io, dl[i]);
		}
}

/* The term's postings follow in bt: df of them, with position records when bt->haspos */
static void
out_begin_term(MergeOut *mo, Batch *bt, const char *term, int len, uint64 df)
{
	bt->n = 0;
	bt->termpos = 0;
	if (mo->sw)
	{
		bt->pw = sift_segw_begin_term(mo->sw, term, len);
		return;
	}
	io_varint(&mo->io, len);
	io_put(&mo->io, term, len);
	io_varint(&mo->io, df);
	io_varint(&mo->io, bt->haspos);
	bt->prev = 0;
}

/* All runs' docs in key order (the runs' key ranges may interleave) */
static void
merge_docs(MergeOut *mo, RunReader *rd, int nruns)
{
	binaryheap *heap = binaryheap_allocate(nruns, doc_heap_cmp, NULL);
	SiftKey		keys[BATCH];
	uint16		dl[BATCH];
	int			n = 0;

	for (int i = 0; i < nruns; i++)
		if (reader_next_doc(&rd[i]))
			binaryheap_add_unordered(heap, PointerGetDatum(&rd[i]));
	binaryheap_build(heap);
	while (!binaryheap_empty(heap))
	{
		RunReader  *top = (RunReader *) DatumGetPointer(binaryheap_first(heap));

		keys[n] = top->dkey;
		dl[n++] = top->ddl;
		if (reader_next_doc(top))
			binaryheap_replace_first(heap, PointerGetDatum(top));
		else
			binaryheap_remove_first(heap);
		if (n == BATCH)
		{
			out_docs(mo, keys, dl, n);
			n = 0;
			CHECK_FOR_INTERRUPTS();
		}
	}
	if (n > 0)
		out_docs(mo, keys, dl, n);
	binaryheap_free(heap);
}

static inline void
record_put(RunReader *rd, uint8 c)
{
	if (rd->prlen == rd->prcap)
	{
		rd->prcap = Max(256, rd->prcap * 2);
		rd->prec = rd->prec ? repalloc_huge(rd->prec, rd->prcap) : MemoryContextAllocHuge(CurrentMemoryContext, rd->prcap);
	}
	rd->prec[rd->prlen++] = c;
}

static inline uint32
record_varint(RunReader *rd)
{
	uint32		x = 0;

	for (int s = 0;; s += 7)
	{
		uint8		c = io_byte(&rd->io);

		record_put(rd, c);
		x |= (uint32) (c & 0x7f) << s;
		if (!(c & 0x80))
			return x;
	}
}

/* The position record of the current posting (see bm25/analyze.h) into rd->prec */
static void
read_record(RunReader *rd)
{
	if (record_varint(rd) & 1)
	{
		uint32		n = record_varint(rd);

		for (uint32 i = 0; i <= n; i++)
			record_varint(rd);
	}
}

/* A run's postings of the current term */
static bool
post_next(RunReader *rd)
{
	uint64		v;

	if (rd->pleft == 0)
		return false;
	v = io_get_varint(&rd->io);
	rd->ptf = (v & 1) ? io_byte(&rd->io) : 1;
	rd->pkey += v >> 1;
	rd->pdl = io_get_varint(&rd->io);
	rd->prlen = 0;
	if (rd->thaspos)
		read_record(rd);
	rd->pleft--;
	return true;
}

/*
 * Postings of the n runs positioned on one term in key order, in batches;
 * act is scratch for n runs. A run drains while its key stays below the
 * others', so runs with disjoint ranges cost one pass each.
 */
static void
merge_postings(Batch *bt, RunReader **cur, int n, RunReader **act)
{
	int			live = 0;

	for (int i = 0; i < n; i++)
	{
		cur[i]->pleft = cur[i]->df;
		cur[i]->pkey = 0;
		if (post_next(cur[i]))
			act[live++] = cur[i];
	}
	while (live > 0)
	{
		int			m = 0;
		SiftKey		limit = PG_UINT64_MAX;
		RunReader  *r;

		for (int i = 1; i < live; i++)
			if (act[i]->pkey < act[m]->pkey)
				m = i;
		for (int i = 0; i < live; i++)
			if (i != m)
				limit = Min(limit, act[i]->pkey);
		r = act[m];
		for (;;)
		{
			batch_put(bt, r->pkey, r->ptf, r->pdl, r->prec, r->prlen);
			if (!post_next(r))
			{
				act[m] = act[--live];
				break;
			}
			if (r->pkey >= limit)
				break;
		}
		CHECK_FOR_INTERRUPTS();
	}
}

/* Opens the runs for reading; the docs and the positions of every run are known by then */
static RunReader *
open_readers(SiftRun **runs, int nruns, Size budget, bool *positions, uint64 *ndocs)
{
	RunReader  *rd = palloc0(nruns * sizeof(RunReader));

	*positions = true;
	*ndocs = 0;
	for (int i = 0; i < nruns; i++)
	{
		rd[i].idx = i;
		rd[i].io.file = run_open(runs[i]);
		rd[i].io.cap = reader_buf(budget, nruns);
		rd[i].io.buf = palloc(rd[i].io.cap);
		if (BufFileSeek(rd[i].io.file, 0, 0, SEEK_SET) != 0)
			elog(ERROR, "sift: could not rewind a build run");
		rd[i].dleft = io_get_varint(&rd[i].io);
		rd[i].haspos = io_get_varint(&rd[i].io);
		*ndocs += rd[i].dleft;
		*positions &= rd[i].haspos;
	}
	return rd;
}

/* Merges the docs and the terms of the opened runs into mo */
static void
merge_readers(RunReader *rd, int nruns, bool positions, MergeOut *mo)
{
	RunReader **cur = palloc(nruns * sizeof(RunReader *));
	RunReader **act = palloc(nruns * sizeof(RunReader *));
	binaryheap *heap = binaryheap_allocate(nruns, reader_cmp, NULL);
	Batch	   *bt = palloc0(sizeof(Batch));
	uint32		nterms = 0;

	bt->run = mo->sw ? NULL : &mo->io;
	merge_docs(mo, rd, nruns);
	for (int i = 0; i < nruns; i++)
	{
		reader_next_term(&rd[i]);
		if (rd[i].len)
			binaryheap_add_unordered(heap, PointerGetDatum(&rd[i]));
	}
	binaryheap_build(heap);
	while (!binaryheap_empty(heap))
	{
		char		term[SIFT_MAX_TERM_BYTES];
		int			len = ((RunReader *) DatumGetPointer(binaryheap_first(heap)))->len,
					k = 0;
		uint64		df = 0;

		memcpy(term, ((RunReader *) DatumGetPointer(binaryheap_first(heap)))->term, len);
		while (!binaryheap_empty(heap))
		{
			RunReader  *top = (RunReader *) DatumGetPointer(binaryheap_first(heap));

			if (sift_term_cmp(top->term, top->len, term, len) != 0)
				break;
			cur[k++] = top;
			binaryheap_remove_first(heap);
		}
		/* a term whose postings lack the records in some run (inline there) keeps none */
		bt->haspos = positions;
		for (int i = 0; i < k; i++)
		{
			bt->haspos &= cur[i]->thaspos;
			df += cur[i]->df;
		}
		out_begin_term(mo, bt, term, len, df);
		merge_postings(bt, cur, k, act);
		batch_flush(bt);
		if (mo->sw)
			sift_segw_end_term(mo->sw);
		else
			run_note_term(mo->run, len, df, bt->termpos);
		for (int i = 0; i < k; i++)
		{
			reader_next_term(cur[i]);
			if (cur[i]->len)
				binaryheap_add(heap, PointerGetDatum(cur[i]));
		}
		TEST_MERGE_TERM(++nterms, df);
		if (nterms % 1024 == 0)
			CHECK_FOR_INTERRUPTS();
	}
}

/* Merges the runs into a new one of the home's files and closes them */
SiftRun *
sift_merge_to_run(SiftRun **runs, int nruns, Size budget, RunHome *home)
{
	SiftRun    *out = run_new(CurrentMemoryContext, home->fileset, home->worker, home->nfiles++);
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run merge", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	MergeOut	mo = {.run = out,.io = {.file = out->file,.buf = palloc(IO_BUF)}};
	bool		positions;
	RunReader  *rd = open_readers(runs, nruns, budget, &positions, &out->ndocs);

	io_varint(&mo.io, out->ndocs);
	io_varint(&mo.io, positions);
	merge_readers(rd, nruns, positions, &mo);
	io_varint(&mo.io, 0);
	io_flush(&mo.io);
	for (int i = 0; i < nruns; i++)
	{
		run_close(runs[i]);
		pfree(runs[i]);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return out;
}

static int
run_size_cmp(const void *a, const void *b)
{
	return pg_cmp_u64((*(SiftRun *const *) b)->ndocs, (*(SiftRun *const *) a)->ndocs);
}

/* Merges the smallest runs, as few as it takes, until target are left; returns their number */
int
sift_shrink_runs(SiftRun **runs, int nruns, int target, Size budget, RunHome *home)
{
	int			fanin = sift_merge_fanin(budget);

	while (nruns > target)
	{
		int			k = Min(fanin, nruns - target + 1);

		qsort(runs, nruns, sizeof(SiftRun *), run_size_cmp);
		runs[nruns - k] = sift_merge_to_run(runs + nruns - k, k, budget, home);
		nruns -= k - 1;
	}
	return nruns;
}

void
sift_runs_write(SiftRun **runs, int nruns, SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref,
				SiftSegEntry *entry)
{
	RunHome		tmp = {0};
	Size		budget = (Size) maintenance_work_mem * 1024;
	MemoryContext cxt;
	MemoryContext old;
	MergeOut	mo = {0};
	bool		positions;
	uint64		ndocs;
	RunReader  *rd;

	/* more runs than readers fit the budget: merge the smallest into temporary ones first */
	nruns = sift_shrink_runs(runs, nruns, sift_merge_fanin(budget), budget, &tmp);
	cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run merge", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	rd = open_readers(runs, nruns, budget, &positions, &ndocs);
	/* the segment has positions when every run does */
	mo.sw = sift_segw_begin(wt, gen, ref, cxt, positions);
	/* the merged runs hold a term of any size: the posting writer takes about 10 bytes of each posting, half the budget at most */
	sift_segw_spool(mo.sw, sift_seg_postings(budget) / 2);
	merge_readers(rd, nruns, positions, &mo);
	sift_segw_finish(mo.sw, entry);
	for (int i = 0; i < nruns; i++)
		run_close(runs[i]);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

/* ---------- runs of segments (merge input) ---------- */

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
	run_note_term(ds->run, len, info->df, haspos ? info->pos_len : 0);
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
	if (ds.ndocs != 0)
		elog(ERROR, "sift: segment %llu lists %llu live docs but streamed another number", ULL(seg->entry.gen), ULL(ndocs));
	io_varint(&ds.io, 0);
	io_flush(&ds.io);
	pfree(ds.io.buf);
	MemoryContextSwitchTo(old);
	return run;
}
