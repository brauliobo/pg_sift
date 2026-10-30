/*
 * bm25/build_runs.c - the build: the directory entry of a segment, its limits, the runs of a group, the spill, the merge and the end of a segment,
 * and the documents of a page (sort, order check, analysis, flush).
 */
#include "build_int.h"

/* Reserves the next directory entry; its generation is the entry's 1-based position */
static SiftSegEntry *
next_entry(BuildState *bs, uint64 *gen)
{
	SIFT_INVARIANT(bs->nsegs < SIFT_MAX_SEGMENTS, "sift: build of \"%s\" ran out of directory entries", RelationGetRelationName(bs->index));
	*gen = bs->nsegs + 1;
	return &bs->segs[bs->nsegs++];
}

/*
 * Limits of a segment written from runs: what its largest term takes of the memory (see seg_postings), of
 * the postings of a term and of the dictionary (the writers fail beyond SIFT_BLOB_MAX and SIFT_DICT_MAX).
 */
typedef struct SegLimits
{
	uint64		df;
	uint64		blob;
	uint64		dict;
} SegLimits;

static SegLimits
seg_limits(Size budget)
{
	return (SegLimits) {sift_seg_postings(budget), SIFT_BLOB_MAX, SIFT_DICT_MAX};
}

/* What the runs of a segment add up to, an upper bound of what the merged run takes */
typedef struct RunSum
{
	uint64		df;
	uint64		blob;
	uint64		dict;
} RunSum;

static RunSum
sum_with(RunSum sum, const SiftRun *run)
{
	sum.df += run->maxdf;
	sum.blob += run->maxblob;
	sum.dict += run->dict;
	return sum;
}

static bool
sum_fits(RunSum sum, const SegLimits *lim)
{
	return sum.df <= lim->df && sum.blob <= lim->blob && sum.dict <= lim->dict;
}

/*
 * The newest runs of one level are a block, which waits until it has fan-in runs: if no more come they are read by
 * the merge that writes the segment, with no pass of their own. Then its runs merge in groups of as many in a row as
 * fit a segment's limits (at most fan-in), each group into a run of the next level; a run that is alone in its
 * group, as large as it gets, is sealed: it stays as it is. Only runs of one level merge, so a posting is
 * merged a number of times logarithmic in the runs.
 */
#define SEALED_LEVEL	INT_MAX

static int
block_runs(SiftRun **runs, int nruns)
{
	int			m = 1;

	if (runs[nruns - 1]->level == SEALED_LEVEL)
		return 0;
	while (m < nruns && runs[nruns - 1 - m]->level == runs[nruns - 1]->level)
		m++;
	return m;
}

static int
group_runs(SiftRun **runs, int first, int nruns, int fanin, const SegLimits *lim)
{
	RunSum		sum = {0};
	int			last = first;

	for (; last < nruns && last - first < fanin && sum_fits(sum_with(sum, runs[last]), lim); last++)
		sum = sum_with(sum, runs[last]);
	return Max(last, first + 1);
}

/* Merges the block of the m newest runs in groups, as above; returns the number of runs left */
static int
merge_block(BuildState *bs, int m, int fanin, const SegLimits *lim)
{
	int			first = bs->nruns - m;
	int			level = bs->runs[first]->level + 1;
	int			w = first;

	for (int i = first, last; i < bs->nruns; i = last)
	{
		SiftRun    *run = bs->runs[i];
		MemoryContext old = MemoryContextSwitchTo(bs->cxt);

		last = group_runs(bs->runs, i, bs->nruns, fanin, lim);
		if (last - i > 1)
			run = sift_merge_to_run(bs->runs + i, last - i, bs->budget, &bs->home);
		MemoryContextSwitchTo(old);
		run->level = last - i > 1 ? level : SEALED_LEVEL;
		bs->runs[w++] = run;
	}
	return w;
}

/*
 * Spills the buffer to a run, then merges its block of runs while it is a fan-in long (merge_block). A participant keeps
 * few runs (their files and buffers fit the budget) and none a hot term would make larger than a segment
 * takes: with the table size the runs that stay are the segments, not levels of merges the term holds whole.
 */
void
spill(BuildState *bs)
{
	int			fanin = sift_merge_fanin(bs->budget);
	SegLimits	lim = seg_limits(bs->budget);
	SiftRun    *run;
	int			m;
	int			phase = bs->phase;

	MEM_SAMPLE();
	progress_phase(bs, PHASE_SPILL);
	if (bs->nruns + 1 >= bs->maxruns)
	{
		bs->maxruns *= 2;
		bs->runs = repalloc(bs->runs, bs->maxruns * sizeof(SiftRun *));
	}
	if (bs->home.fileset == NULL)
		run = sift_inv_spill(bs->inv);
	else
		run = sift_inv_spill_shared(bs->inv, bs->home.fileset, bs->home.worker, bs->home.nfiles++);
	bs->runs[bs->nruns++] = run;
	while ((m = block_runs(bs->runs, bs->nruns)) >= fanin)
		bs->nruns = merge_block(bs, m, fanin, &lim);
	progress_phase(bs, phase);
}

/* End of the runs from first on that go to one segment: their largest terms fit the limits together */
static int
group_end(SiftRun **runs, int first, int nruns, const SegLimits *lim)
{
	RunSum		sum = sum_with((RunSum) {0}, runs[first]);
	int			last = first + 1;

	for (; last < nruns && sum_fits(sum_with(sum, runs[last]), lim); last++)
		sum = sum_with(sum, runs[last]);
	return last;
}

static int
count_groups(SiftRun **runs, int nruns, const SegLimits *lim)
{
	int			n = 0;

	for (int first = 0; first < nruns; first = group_end(runs, first, nruns, lim))
		n++;
	return n;
}

/* After the scan the progress counts the docs of the segments written, of all that were analyzed */
static void
progress_written(BuildState *bs, const SiftSegEntry *entry)
{
	bs->written += entry->ndocs;
	if (bs->scanned)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, bs->written);
}

/* Writes the docs gathered since the last boundary as one or more segments */
void
end_segment(BuildState *bs)
{
	int			first = 0;
	uint64		gen;
	SiftSegEntry *entry;
	SegLimits	lim = seg_limits(bs->budget);
	/* the entries the segments still to come (one per forced boundary, and the last) leave */
	int			room = SIFT_MAX_SEGMENTS - bs->nsegs - (bs->nbounds - bs->nextbound);

	int			phase = bs->phase;

	progress_phase(bs, PHASE_MERGE);
	if (bs->nruns == 0)
	{
		if (!sift_inv_empty(bs->inv))
		{
			entry = next_entry(bs, &gen);
			progress_phase(bs, PHASE_WRITE);
			sift_inv_write(bs->inv, bs->wt, gen, &bs->ref, entry);
			progress_written(bs, entry);
		}
		progress_phase(bs, bs->scanned ? PHASE_WRITE : phase);
		return;
	}
	if (!sift_inv_empty(bs->inv))
		spill(bs);
	ereport(DEBUG2, errmsg("sift index \"%s\": merging %d runs into segments", RelationGetRelationName(bs->index), bs->nruns));
	/*
	 * Each segment merges runs whose largest terms fit the budget together. The directory holds a
	 * number of segments, though: a term is encoded whole, so when the budget would need more, fewer
	 * and larger segments are written, which take more memory than the budget in their largest terms.
	 */
	while (count_groups(bs->runs, bs->nruns, &lim) > room)
	{
		SIFT_INVARIANT(lim.df <= PG_INT64_MAX / 2, "sift: index \"%s\" would need more than %d segments", RelationGetRelationName(bs->index),
					   SIFT_MAX_SEGMENTS);
		lim.df *= 2;
	}
	while (first < bs->nruns)
	{
		int			last = group_end(bs->runs, first, bs->nruns, &lim);
		RunHome		tmp = {0};
		int			n;

		progress_phase(bs, PHASE_MERGE);
		n = sift_shrink_runs(bs->runs + first, last - first, sift_merge_fanin(bs->budget), bs->budget, &tmp);
		entry = next_entry(bs, &gen);
		progress_phase(bs, PHASE_WRITE);
		sift_runs_write(bs->runs + first, n, bs->wt, gen, &bs->ref, entry);
		progress_written(bs, entry);
		first = last;
	}
	bs->nruns = 0;
	progress_phase(bs, bs->scanned ? PHASE_WRITE : phase);
}

static int
pagedoc_cmp(const void *a, const void *b)
{
	return ItemPointerCompare(&((PageDoc *) a)->tid, &((PageDoc *) b)->tid);
}

/* The docs of a page come in tid order but for HOT chains, which report their root's tid: sort only when they do not */
static void
sort_page(PageDoc *docs, int ndocs)
{
	for (int i = 1; i < ndocs; i++)
		if (ItemPointerGetOffsetNumber(&docs[i - 1].tid) > ItemPointerGetOffsetNumber(&docs[i].tid))
		{
			qsort(docs, ndocs, sizeof(PageDoc), pagedoc_cmp);
			return;
		}
}

/* Docs are added in ascending key order: the buffer is spilled when the next doc is not above the last one (a page cut short by the text limit goes on with lower tids) */
void
check_order(BuildState *bs, ItemPointer tid)
{
	if (!sift_inv_after(bs->inv, tid))
		spill(bs);
}


/* Analyzes the buffered docs of one heap page in tid order */
void
flush_page(BuildState *bs)
{
	sort_page(bs->docs, bs->ndocs);
	if (bs->ndocs > 0)
		check_order(bs, &bs->docs[0].tid);
	for (int i = 0; i < bs->ndocs; i++)
		analyze_doc(bs, &bs->docs[i].tid, bs->text.data + bs->docs[i].off, bs->docs[i].len);
	bs->ndocs = 0;
	resetStringInfo(&bs->text);
	sift_analyzer_reset(bs->an);
	progress_docs(bs, false);
	CHECK_FOR_INTERRUPTS();
}

/*
 * The text of the docs of a heap page is copied until the page is done, to analyze them in tid order (a
 * HOT chain reports its root's tid). A page of large docs does not hold them all: past this many bytes
 * what is buffered is analyzed, and a doc over it is analyzed at once. A later doc of the page with a
 * lower tid then starts a new run (check_order).
 */
Size
page_text_max(const BuildState *bs)
{
	return TEST_KNOB(sift_test_page_text, Min(Max(bs->budget / 4, 1024 * 1024), 256 * 1024 * 1024));
}
