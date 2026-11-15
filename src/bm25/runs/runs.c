/*
 * bm25/runs/runs.c - sorted runs of a build or a merge: their files, the shrink of a set of runs
 * the write of the segment from runs, the close
 * the readers, the merge and the cuts are in the files next to it
 */
#include "bm25/runs/runs_priv.h"

#ifdef SIFT_TESTING
int			sift_test_run_docs = 0;

int			sift_test_fanin = 0;

int			sift_test_segment_postings = 0;

int			sift_test_pool_blocks = 0;

int			sift_test_range_bytes = 0;

void		(*sift_test_merge_term) (uint32 nterms, uint64 df) = NULL;
#endif

/* The term of a run: len bytes, df postings and posbytes bytes of position records */
void
runs_run_note_term(SiftRun *run, int len, uint64 df, uint64 posbytes)
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
BufFile *
runs_run_open(SiftRun *run)
{
	if (run->file == NULL)
		run->file = BufFileOpenFileSet(&run->fileset->fs, sift_run_file(run->worker, run->id), O_RDONLY, false);
	return run->file;
}

/* Closes a merged run; a fileset's file goes too, as the leader or the worker has no more use for it */
static void
run_close(SiftRun *run)
{
	if (run->file)
		BufFileClose(run->file);
	if (run->fileset)
		BufFileDeleteFileSet(&run->fileset->fs, sift_run_file(run->worker, run->id), false);
}

/* Closes the handle of a run another process wrote; the file stays for the next process to read */
void
runs_run_release(SiftRun *run)
{
	if (run->file)
		BufFileClose(run->file);
	run->file = NULL;
}

/* Runs one merge reads at a time: a reader each (the smallest has two blocks), within half the budget */
int
sift_merge_fanin(Size budget)
{
	return Max(2, TEST_KNOB(sift_test_fanin, Min(MAX_MERGE_FANIN, Max(8, budget / 2 / (2 * BLCKSZ)))));
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
	RunReader  *rd = runs_open_readers(runs, nruns, budget, &positions, &out->ndocs);

	io_varint(&mo.io, out->ndocs);
	io_varint(&mo.io, positions);
	runs_merge_readers(rd, nruns, positions, &mo);
	run_finish(out, &mo.io);
	for (int i = 0; i < nruns; i++)
	{
		out->sum_dl += runs[i]->sum_dl;
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

/* A segment writer that also stores the values of the INCLUDE columns, which the process that merges the docs reads from the heap */
SiftSegmentWriter *
runs_segw_begin(SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref, MemoryContext mcxt, bool positions)
{
	SiftSegmentWriter *sw = sift_segw_begin(wt, gen, ref, mcxt, positions);
	SiftAttrSrc *as = sift_attr_src(wt, mcxt);

	if (as != NULL)
		sift_segw_attrs(sw, as);
	return sw;
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
	rd = runs_open_readers(runs, nruns, budget, &positions, &ndocs);
	/* the segment has positions when every run does */
	mo.sw = runs_segw_begin(wt, gen, ref, cxt, positions);
	/* the merged runs hold a term of any size: the posting writer takes about 10 bytes of each posting, half the budget at most */
	sift_segw_spool(mo.sw, sift_seg_postings(budget) / 2);
	runs_merge_readers(rd, nruns, positions, &mo);
	sift_segw_finish(mo.sw, entry);
	for (int i = 0; i < nruns; i++)
		run_close(runs[i]);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}

void
sift_runs_close(SiftRun **runs, int nruns)
{
	for (int i = 0; i < nruns; i++)
		run_close(runs[i]);
}
