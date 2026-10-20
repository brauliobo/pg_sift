/*
 * bm25/build.c - the PostgreSQL side of the build: heap scan, parallel workers and leader, ambuild and ambuildempty
 * The inverted buffer, the runs and their merge are shared with the other hosts (bm25/inv.c, bm25/runs.c).
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/parallel.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "common/int.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "storage/sharedfileset.h"
#include "tcop/tcopprot.h"
#include "utils/backend_progress.h"
#include "utils/backend_status.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "bm25/am.h"
#include "bm25/build.h"
#include "sift_parbuild.h"
#include "table/table.h"
#include "sift_visibility.h"

#define BATCH			1024
#define ULL(x)			((unsigned long long) (x))

#ifdef SIFT_TESTING
int			sift_test_build_peak = 0;
int			sift_test_page_text = 0;
int			sift_test_fail_merge = 0;
int			sift_test_fail_who = 0;
int			sift_test_phase_gate = 0;

/* The most memory (kB) the contexts of the build allocated, sampled where a build is at its largest */
static MemoryContext mem_root;
static MemoryContextCallback mem_root_cb;
static int	test_merges;		/* merges so far of the build of this process */

static void
mem_sample(void)
{
	if (mem_root)
		sift_test_build_peak = Max(sift_test_build_peak, (int) Min(MemoryContextMemAllocated(mem_root, true) / 1024, INT_MAX));
}

/* The sift_test_fail_merge'th merge of the build participants selected by sift_test_fail_who is cancelled after a few terms */
static void
test_merge_term(uint32 nterms, uint64 df)
{
	if (df >= 64)
		mem_sample();
	if (nterms != 3 || sift_test_fail_merge == 0 ||
		(sift_test_fail_who != 0 && (sift_test_fail_who == 1) != (ParallelWorkerNumber < 0)))
		return;
	if (++test_merges == sift_test_fail_merge)
		ereport(ERROR, (errcode(ERRCODE_QUERY_CANCELED), errmsg("canceling statement due to sift.test_fail_merge")));
}

static void
mem_root_gone(void *arg)
{
	mem_root = NULL;
	sift_test_merge_term = NULL;
}

static void
mem_watch(MemoryContext cxt)
{
	mem_root = cxt;
	test_merges = 0;
	sift_test_merge_term = test_merge_term;
	mem_root_cb.func = mem_root_gone;
	MemoryContextRegisterResetCallback(cxt, &mem_root_cb);
}
#define MEM_SAMPLE()	mem_sample()

/* With sift_test_phase_gate set, entering a phase of the build waits for the advisory lock gate + phase (held by the test) */
static void
test_phase_gate(int phase)
{
	if (sift_test_phase_gate > 0)
	{
		Datum		key = Int64GetDatum(sift_test_phase_gate + phase);

		DirectFunctionCall1(pg_advisory_lock_int8, key);
		DirectFunctionCall1(pg_advisory_unlock_int8, key);
	}
}
#define TEST_PHASE_GATE(phase)	test_phase_gate(phase)
#else
#define MEM_SAMPLE()	((void) 0)
#define TEST_PHASE_GATE(phase)	((void) 0)
#endif

/* ---------- ambuild ---------- */

typedef struct PageDoc
{
	ItemPointerData tid;
	uint32		off;			/* into the page's text arena */
	uint32		len;
} PageDoc;

typedef struct BuildState
{
	Relation	index;
	MemoryContext cxt;
	MemoryContext tmpcxt;
	SiftAnalyzer *an;
	SiftInvBuf *inv;
	Size		budget;
	Size		page_text;		/* bytes of text of a heap page held before they are analyzed (see page_text_max) */
	Size		small_doc;		/* docs of fewer bytes of text always fit the buffer that is not full */
	SiftWriteTarget *wt;
	SiftScoreRef ref;
	/* docs of the current heap page, sorted by offset before analysis */
	BlockNumber page;
	PageDoc    *docs;
	int			ndocs;
	int			maxdocs;
	StringInfoData text;
	/* forced segment boundaries (build_segments) */
	BlockNumber *bounds;
	int			nbounds;
	int			nextbound;
	/* runs of the current segment */
	SiftRun   **runs;
	int			nruns;
	int			maxruns;
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
	int			nsegs;
	double		indexed;
	uint32		analyzer_hash;
	bool		positions;
	/* a parallel worker's runs go to files of the leader's fileset */
	RunHome		home;
	/* progress (pg_stat_progress_create_index): the phase, docs reported, scan done, docs of the segments written */
	int			phase;
	double		reported;
	bool		scanned;
	uint64		written;
} BuildState;

/* Subphases of the build, named by sift_ambuildphasename */
#define PHASE_SCAN		2
#define PHASE_SPILL		3
#define PHASE_MERGE		4
#define PHASE_WRITE		5
#define PROGRESS_DOCS	1024		/* docs analyzed between two reports of the scan */

char *
sift_ambuildphasename(int64 phase)
{
	switch (phase)
	{
		case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
			return "initializing";
		case PHASE_SCAN:
			return "scanning table";
		case PHASE_SPILL:
			return "spilling runs";
		case PHASE_MERGE:
			return "merging runs";
		case PHASE_WRITE:
			return "writing segments";
		default:
			return NULL;
	}
}

/* Only the leader (or a serial build) has a progress to report; a worker's docs go to it with progress_docs */
static void
progress_phase(BuildState *bs, int phase)
{
	if (bs->phase == phase)
		return;
	bs->phase = phase;
	if (IsParallelWorker())
		return;
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, phase);
	TEST_PHASE_GATE(phase);
}

/* The docs analyzed since the last report, at least PROGRESS_DOCS of them unless all is set */
static void
progress_docs(BuildState *bs, bool all)
{
	if (bs->indexed - bs->reported < (all ? 1 : PROGRESS_DOCS))
		return;
	pgstat_progress_parallel_incr_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64) (bs->indexed - bs->reported));
	bs->reported = bs->indexed;
}

/* Reserves the next directory entry; its generation is the entry's 1-based position */
static SiftSegEntry *
next_entry(BuildState *bs, uint64 *gen)
{
	if (bs->nsegs == SIFT_MAX_SEGMENTS)
		elog(ERROR, "sift: build of \"%s\" ran out of directory entries", RelationGetRelationName(bs->index));
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
static void
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
static void
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
		if (lim.df > PG_INT64_MAX / 2)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" would need more than %d segments", RelationGetRelationName(bs->index), SIFT_MAX_SEGMENTS)));
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
static void
check_order(BuildState *bs, ItemPointer tid)
{
	if (!sift_inv_after(bs->inv, tid))
		spill(bs);
}

/* Analyzes a doc into the buffer, which is spilled first when a large doc does not fit it */
static inline void
analyze_doc(BuildState *bs, ItemPointer tid, const char *text, int len)
{
	SiftTermFreq *terms;
	uint32		dl;
	int			n = sift_analyze_doc(bs->an, text, len, &terms, &dl);

	if (len >= bs->small_doc && !sift_inv_empty(bs->inv) && !sift_inv_fits(bs->inv, n, len))
		spill(bs);
	bs->indexed++;
	if (sift_inv_add(bs->inv, tid, terms, n, dl))
		spill(bs);
}

/* Analyzes the buffered docs of one heap page in tid order */
static void
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
static Size
page_text_max(const BuildState *bs)
{
	return TEST_KNOB(sift_test_page_text, Min(Max(bs->budget / 4, 1024 * 1024), 256 * 1024 * 1024));
}

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	BuildState *bs = state;
	MemoryContext old;
	text	   *t;
	bool		detoasted;
	int			len;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);

	if (isnull[0])
		return;
	if (blk != bs->page)
	{
		flush_page(bs);
		bs->page = blk;
		for (; bs->nextbound < bs->nbounds && blk >= bs->bounds[bs->nextbound]; bs->nextbound++)
			end_segment(bs);
	}
	old = MemoryContextSwitchTo(bs->tmpcxt);
	t = DatumGetTextPP(values[0]);
	detoasted = (Pointer) t != DatumGetPointer(values[0]);
	len = VARSIZE_ANY_EXHDR(t);
	if (bs->text.len > 0 && bs->text.len + len > bs->page_text)
		flush_page(bs);
	if (len > bs->page_text)
	{
		check_order(bs, tid);
		analyze_doc(bs, tid, VARDATA_ANY(t), len);
		sift_analyzer_reset(bs->an);
	}
	else
	{
		if (bs->ndocs == bs->maxdocs)
		{
			bs->maxdocs *= 2;
			bs->docs = repalloc(bs->docs, bs->maxdocs * sizeof(PageDoc));
		}
		bs->docs[bs->ndocs++] = (PageDoc) {*tid, bs->text.len, len};
		if (bs->text.len + len < bs->text.maxlen)
		{
			memcpy(bs->text.data + bs->text.len, VARDATA_ANY(t), len);
			bs->text.len += len;
		}
		else
			appendBinaryStringInfo(&bs->text, VARDATA_ANY(t), len);
	}
	MemoryContextSwitchTo(old);
	if (detoasted || len > bs->page_text)
		MemoryContextReset(bs->tmpcxt);
}

static void
publish(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	BuildState *bs = arg;

	meta->next_gen = bs->nsegs + 1;
	memcpy(dir->segs, bs->segs, bs->nsegs * sizeof(SiftSegEntry));
	dir->nsegs = bs->nsegs;
	for (int i = 0; i < bs->nsegs; i++)
	{
		meta->ndocs += bs->segs[i].ndocs;
		meta->sum_dl += bs->segs[i].sum_dl;
	}
}

static void
init_bounds(BuildState *bs, Relation heap, int nsegments)
{
	BlockNumber nblocks = sift_vis_nblocks(heap);

	bs->bounds = palloc(Max(nsegments, 1) * sizeof(BlockNumber));
	for (int i = 1; i < nsegments; i++)
	{
		BlockNumber b = (BlockNumber) ((uint64) nblocks * i / nsegments);

		if (b > 0 && (bs->nbounds == 0 || b > bs->bounds[bs->nbounds - 1]))
			bs->bounds[bs->nbounds++] = b;
	}
}

/* Allocates the build state; the caller switches to bs->cxt for the scan */
static BuildState *
build_begin(Relation index, Size budget)
{
	const SiftOptions *opts = sift_index_options(index);
	BuildState *bs = palloc0(sizeof(BuildState));
	SiftAnalyzerConfig cfg;
	MemoryContext old;

	sift_analyzer_config_from_options(opts, &cfg);
	bs->index = index;
	bs->analyzer_hash = cfg.hash;
	bs->cxt = AllocSetContextCreate(CurrentMemoryContext, "sift build", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(bs->cxt);
	bs->tmpcxt = AllocSetContextCreate(bs->cxt, "sift build tuple", ALLOCSET_DEFAULT_SIZES);
	bs->an = sift_analyzer_create(&cfg, bs->cxt);
	bs->positions = opts->positions && !cfg.keyword;
	sift_analyzer_set_positions(bs->an, bs->positions);
	bs->budget = budget;
	bs->inv = sift_inv_create(bs->budget, bs->cxt, bs->positions);
	bs->small_doc = sift_inv_small_doc(bs->inv);
	bs->page_text = page_text_max(bs);
	bs->ref = (SiftScoreRef) {(float4) opts->k1, (float4) opts->b, 0};
	bs->page = InvalidBlockNumber;
	bs->maxdocs = 64;
	bs->docs = palloc(bs->maxdocs * sizeof(PageDoc));
	initStringInfo(&bs->text);
	bs->maxruns = 16;
	bs->runs = palloc(bs->maxruns * sizeof(SiftRun *));
	MemoryContextSwitchTo(old);
#ifdef SIFT_TESTING
	mem_watch(bs->cxt);
#endif
	return bs;
}

/* Scans the heap, or this participant's share of a parallel scan, into the inverted buffer and its runs */
static double
build_scan(BuildState *bs, Relation heap, IndexInfo *info, TableScanDesc scan, bool progress)
{
	double		reltuples = table_index_build_scan(heap, bs->index, info, scan != NULL, progress, build_cb, bs, scan);

	flush_page(bs);
	progress_docs(bs, true);
	return reltuples;
}

/* ---------- parallel ambuild ---------- */

/*
 * Every participant scans its share of a parallel heap scan (ascending keys,
 * interleaved with the others') into sorted runs. The workers' runs are files
 * of a shared fileset; the leader merges them with its own into the segments
 * a serial build would write.
 */
#define SCAN_OF(shared)	((ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(SiftBuildShared))))

typedef struct SiftBuildShared
{
	SiftParCommon common;
	int			nparticipants;
	Size		budget;				/* per participant */
	SharedFileSet fileset;
	slock_t		mutex;
	double		reltuples;
	double		indexed;
	int			peak_kb;			/* the most memory a participant allocated (SIFT_TESTING) */
	/* the parallel table scan descriptor follows */
} SiftBuildShared;

/* The most runs a worker hands over (the leader opens each as it merges it) */
#define MAX_PART_RUNS	1024

/* A worker's runs, in the files sift_run_file(worker number, id) */
typedef struct SiftBuildPart
{
	int			nruns;
	struct
	{
		int			id;
		uint64		ndocs;
		uint64		maxdf;
		uint64		maxblob;
		uint64		dict;
	}			run[MAX_PART_RUNS];
} SiftBuildPart;

/*
 * A synchronized scan starts mid-heap and wraps around, but every participant needs ascending keys.
 * The scan decides at initialization, so the setting only matters here.
 */
static void
init_scan(Relation heap, SiftBuildShared *shared, Snapshot snapshot)
{
	int			nest = NewGUCNestLevel();

	set_config_option("synchronize_seqscans", "off", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
	table_parallelscan_initialize(heap, SCAN_OF(shared), snapshot);
	AtEOXact_GUC(false, nest);
}

/* NULL when no worker could be started: the leader then builds alone */
static SiftParLeader *
begin_parallel(Relation heap, Relation index, IndexInfo *info)
{
	int			nworkers = info->ii_ParallelWorkers;
	SiftParLeader *ldr = sift_par_enter(info->ii_Concurrent);
	Size		shared_size = add_size(BUFFERALIGN(sizeof(SiftBuildShared)), table_parallelscan_estimate(heap, ldr->snapshot));
	SiftBuildShared *shared;

	if (!sift_par_setup(ldr, "sift_build_worker", nworkers, shared_size, mul_size(sizeof(SiftBuildPart), nworkers), false))
		return NULL;
	shared = ldr->shared;
	*shared = (SiftBuildShared) {.common = {RelationGetRelid(heap), RelationGetRelid(index), info->ii_Concurrent},
		.nparticipants = nworkers + 1,.budget = (Size) maintenance_work_mem * 1024 / (nworkers + 1)};
	SharedFileSetInit(&shared->fileset, ldr->pcxt->seg);
	SpinLockInit(&shared->mutex);
	init_scan(heap, shared, ldr->snapshot);
	return sift_par_launch(ldr, "sift", index, nworkers) ? ldr : NULL;
}

/* Waits for the workers and adds their runs to the leader's; returns the tuple counts through res */
static void
adopt_runs(SiftParLeader *ldr, BuildState *bs, IndexBuildResult *res)
{
	SiftBuildShared *shared = ldr->shared;
	SiftBuildPart *parts = ldr->parts;

	sift_par_wait(ldr);
	for (int w = 0; w < ldr->pcxt->nworkers_launched; w++)
	{
		for (int i = 0; i < parts[w].nruns; i++)
		{
			SiftRun    *run = palloc(sizeof(SiftRun));

			if (bs->nruns == bs->maxruns)
			{
				bs->maxruns *= 2;
				bs->runs = repalloc(bs->runs, bs->maxruns * sizeof(SiftRun *));
			}
			*run = (SiftRun) {.ndocs = parts[w].run[i].ndocs,.maxdf = parts[w].run[i].maxdf,
				.maxblob = parts[w].run[i].maxblob,.dict = parts[w].run[i].dict,
			.fileset = &shared->fileset,.worker = w,.id = parts[w].run[i].id};
			bs->runs[bs->nruns++] = run;
		}
	}
	res->heap_tuples = shared->reltuples;
	res->index_tuples = shared->indexed;
#ifdef SIFT_TESTING
	sift_test_build_peak = Max(sift_test_build_peak, shared->peak_kb);
#endif
}

static void
add_counts(SiftBuildShared *shared, double reltuples, double indexed)
{
	SpinLockAcquire(&shared->mutex);
	shared->reltuples += reltuples;
	shared->indexed += indexed;
	SpinLockRelease(&shared->mutex);
}

/* Spills what is left, merges the runs down to what a part holds and makes them readable by the leader */
static void
hand_over_runs(BuildState *bs, SiftBuildPart *part)
{
	MemoryContext old = MemoryContextSwitchTo(bs->cxt);

	if (!sift_inv_empty(bs->inv))
		spill(bs);
	bs->nruns = sift_shrink_runs(bs->runs, bs->nruns, MAX_PART_RUNS, bs->budget, &bs->home);
	MemoryContextSwitchTo(old);
	part->nruns = bs->nruns;
	for (int i = 0; i < bs->nruns; i++)
	{
		part->run[i].id = bs->runs[i]->id;
		part->run[i].ndocs = bs->runs[i]->ndocs;
		part->run[i].maxdf = bs->runs[i]->maxdf;
		part->run[i].maxblob = bs->runs[i]->maxblob;
		part->run[i].dict = bs->runs[i]->dict;
		BufFileExportFileSet(bs->runs[i]->file);
		BufFileClose(bs->runs[i]->file);
	}
}

PGDLLEXPORT void sift_build_worker(dsm_segment *seg, shm_toc *toc);

void
sift_build_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;
	SiftBuildShared *shared;
	SiftBuildPart *part;
	BuildState *bs;
	double		reltuples;

	sift_par_worker_begin(&w, toc);
	shared = w.shared;
	part = (SiftBuildPart *) w.parts + ParallelWorkerNumber;
	SharedFileSetAttach(&shared->fileset, seg);
	bs = build_begin(w.index, shared->budget);
	bs->home = (RunHome) {.fileset = &shared->fileset,.worker = ParallelWorkerNumber};
	reltuples = build_scan(bs, w.heap, w.info, sift_table_beginscan_parallel(w.heap, SCAN_OF(shared)), false);
	hand_over_runs(bs, part);
	add_counts(shared, reltuples, bs->indexed);
#ifdef SIFT_TESTING
	MEM_SAMPLE();
	SpinLockAcquire(&shared->mutex);
	shared->peak_kb = Max(shared->peak_kb, sift_test_build_peak);
	SpinLockRelease(&shared->mutex);
#endif
	MemoryContextDelete(bs->cxt);
	sift_par_worker_end(&w);
}

IndexBuildResult *
sift_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	const SiftOptions *opts = sift_index_options(index);
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));
	SiftParLeader *ldr = NULL;
	BuildState *bs;
	MemoryContext old;

	if (heap->rd_tableam != GetHeapamTableAmRoutine() && !sift_tbl_is_table(heap))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift indexes require a heap or sift_table table")));
	if (opts->build_segments == 0 && info->ii_ParallelWorkers > 0)
		ldr = begin_parallel(heap, index, info);
	bs = build_begin(index, (Size) maintenance_work_mem * 1024 / (ldr ? ((SiftBuildShared *) ldr->shared)->nparticipants : 1));
	old = MemoryContextSwitchTo(bs->cxt);
	if (ldr == NULL)
		init_bounds(bs, heap, opts->build_segments);
	bs->wt = sift_wt_begin(index, SIFT_WRITE_BULK);
	if (heap->rd_rel->reltuples > 0)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
	progress_phase(bs, PHASE_SCAN);
	res->heap_tuples = build_scan(bs, heap, info, ldr ? sift_table_beginscan_parallel(heap, SCAN_OF(ldr->shared)) : NULL, true);
	res->index_tuples = bs->indexed;
	if (ldr)
	{
		add_counts(ldr->shared, res->heap_tuples, bs->indexed);
		adopt_runs(ldr, bs, res);
		bs->budget = (Size) maintenance_work_mem * 1024;
	}
	bs->scanned = true;
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) res->index_tuples);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, 0);
	end_segment(bs);
	if (ldr)
		sift_par_end(ldr);
	sift_store_init_meta(bs->wt, opts, bs->analyzer_hash);
	sift_wt_finish(bs->wt);
	if (bs->nsegs > 0)
		sift_dir_update(index, &(SiftDirChange) {.fn = publish,.arg = bs,.extra = InvalidBuffer});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(bs->cxt);
	return res;
}

void
sift_ambuildempty(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_INIT);
	SiftAnalyzerConfig cfg;

	sift_analyzer_config_from_index(index, &cfg);
	sift_store_init_meta(wt, sift_index_options(index), cfg.hash);
	sift_wt_finish(wt);
}
