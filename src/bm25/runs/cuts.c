/*
 * bm25/runs/cuts.c - the cuts of the terms of a set of runs
 * the samples of the runs and the choice of the cuts
 * the write of one range of them
 */
#include "core/host.h"
#include "core/cmp.h"
#include "bm25/runs/runs_priv.h"

bool
sift_runs_splittable(SiftRun **runs, int nruns)
{
	for (int i = 0; i < nruns; i++)
		if (runs[i]->trailer == 0 || runs[i]->fileset == NULL)
			return false;
	return true;
}

static RunSample *
run_samples(RunReader *rd, const SiftRun *run, int *n)
{
	RunSample  *smp;
	uint64		end;

	SIFT_INVARIANT(BufFileSeek(rd->io.file, 0, (off_t) run->trailer, SEEK_SET) == 0, "sift: could not seek in a build run");
	rd->io.pos = rd->io.len = 0;
	*n = io_get_varint(&rd->io);
	end = io_get_varint(&rd->io);
	smp = palloc((*n + 1) * sizeof(RunSample));
	for (int i = 0; i < *n; i++)
	{
		char	   *term;

		smp[i].len = io_get_varint(&rd->io);
		term = palloc(smp[i].len);
		for (int j = 0; j < smp[i].len; j++)
			term[j] = io_byte(&rd->io);
		smp[i].term = term;
		smp[i].off = io_get_varint(&rd->io);
		if (i > 0)
			smp[i - 1].vol = smp[i].off - smp[i - 1].off;
	}
	if (*n > 0)
		smp[*n - 1].vol = end - smp[*n - 1].off;
	return smp;
}

static int
sample_cmp(const void *a, const void *b)
{
	const RunSample *x = a,
			   *y = b;

	return sift_term_cmp(x->term, x->len, y->term, y->len);
}

/* The cuts, in term order, that split the volume of the samples into about target parts, none under min_bytes; the samples are sorted here */
int
runs_choose_cuts(RunSample *all, int nall, uint64 min_bytes, int target, SiftTermCut *cuts)
{
	uint64		total = 0,
				acc = 0;
	int			ncuts = 0,
				nranges;

	for (int i = 0; i < nall; i++)
		total += all[i].vol;
	nranges = (int) Min((uint64) target, Min(total / Max(min_bytes, 1), (uint64) nall));
	if (nranges < 2)
		return 0;
	qsort(all, nall, sizeof(RunSample), sample_cmp);
	for (int i = 0; i < nall && ncuts < nranges - 1; acc += all[i++].vol)
	{
		if (acc * nranges < total * (uint64) (ncuts + 1) || (ncuts > 0 && sift_term_cmp(all[i].term, all[i].len, cuts[ncuts - 1].term, cuts[ncuts - 1].len) <= 0))
			continue;
		cuts[ncuts].len = all[i].len;
		memcpy(cuts[ncuts].term, all[i].term, all[i].len);
		ncuts++;
	}
	return ncuts;
}

int
sift_runs_cuts(SiftRun **runs, int nruns, Size budget, uint64 min_bytes, int target, SiftTermCut *cuts)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run cuts", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	bool		positions;
	uint64		ndocs;
	RunReader  *rd = runs_open_readers(runs, nruns, budget, &positions, &ndocs);
	RunSample  *all = NULL;
	int			nall = 0,
				ncuts;

	for (int i = 0; i < nruns; i++)
	{
		int			n;
		RunSample  *smp = run_samples(&rd[i], runs[i], &n);

		SIFT_REGROW(all, (nall + n) * sizeof(RunSample));
		memcpy(all + nall, smp, n * sizeof(RunSample));
		nall += n;
	}
	ncuts = runs_choose_cuts(all, nall, min_bytes, target, cuts);
	for (int i = 0; i < nruns; i++)
		runs_run_release(runs[i]);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return ncuts;
}

/* Starts the reader at the first term not below lo (NULL: the first): the last sample not above it, and on from there */
static void
reader_to_term(RunReader *rd, const SiftRun *run, const SiftTermCut *lo)
{
	int			n;
	RunSample  *smp = run_samples(rd, run, &n);
	uint64		at = n > 0 ? smp[0].off : 0;

	for (int i = 1; lo && i < n && sift_term_cmp(smp[i].term, smp[i].len, lo->term, lo->len) <= 0; i++)
		at = smp[i].off;
	if (n == 0)
	{
		rd->len = 0;
		return;
	}
	SIFT_INVARIANT(BufFileSeek(rd->io.file, 0, (off_t) at, SEEK_SET) == 0, "sift: could not seek in a build run");
	rd->io.pos = rd->io.len = 0;
	runs_reader_next_term(rd);
	while (lo && rd->len && sift_term_cmp(rd->term, rd->len, lo->term, lo->len) < 0)
	{
		rd->pleft = rd->df;
		rd->pkey = 0;
		while (runs_post_next(rd))
			;
		runs_reader_next_term(rd);
	}
}

SiftSegmentWriter *
sift_runs_write_docs(SiftRun **runs, int nruns, Size budget, SiftWriteTarget *wt, uint64 gen, const SiftScoreRef *ref, MemoryContext mcxt)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run docs", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	MergeOut	mo = {0};
	bool		positions;
	uint64		ndocs;
	RunReader  *rd = runs_open_readers(runs, nruns, budget, &positions, &ndocs);

	mo.sw = runs_segw_begin(wt, gen, ref, mcxt, positions);
	runs_merge_docs(&mo, rd, nruns);
	for (int i = 0; i < nruns; i++)
		runs_run_release(runs[i]);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
	return mo.sw;
}

void
sift_runs_write_range(SiftRun **runs, int nruns, Size budget, const SiftTermCut *lo, const SiftTermCut *hi, SiftSegmentWriter *rw)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift run range", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	MergeOut	mo = {.sw = rw};
	bool		positions;
	uint64		ndocs;
	RunReader  *rd = runs_open_readers(runs, nruns, budget, &positions, &ndocs);

	for (int i = 0; i < nruns; i++)
		reader_to_term(&rd[i], runs[i], lo);
	runs_merge_terms(rd, nruns, positions, &mo, hi);
	for (int i = 0; i < nruns; i++)
		runs_run_release(runs[i]);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cxt);
}
