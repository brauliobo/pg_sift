/*
 * sift_build.c - the PostgreSQL side of the build: heap scan, parallel workers and leader, ambuild and ambuildempty
 * (DESIGN.md (i)). The inverted buffer, the runs and their merge are shared with the other hosts (sift_runs.c).
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/parallel.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "common/int.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "storage/sharedfileset.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "sift_am.h"
#include "sift_build.h"

#define BATCH			1024
#define ULL(x)			((unsigned long long) (x))

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
} BuildState;

/* Reserves the next directory entry; its generation is the entry's 1-based position */
static SiftSegEntry *
next_entry(BuildState *bs, uint64 *gen)
{
	if (bs->nsegs == SIFT_MAX_SEGMENTS)
		elog(ERROR, "sift: build of \"%s\" ran out of directory entries", RelationGetRelationName(bs->index));
	*gen = bs->nsegs + 1;
	return &bs->segs[bs->nsegs++];
}

static bool
tail_is_one_level(SiftRun **runs, int nruns, int k)
{
	if (nruns < k)
		return false;
	for (int i = nruns - k; i < nruns; i++)
		if (runs[i]->level != runs[nruns - 1]->level)
			return false;
	return true;
}

/*
 * Spills the buffer to a run. The last fan-in runs of one level merge into a run of the next, so the
 * runs a participant keeps stay few (their files and buffers fit the budget) and every posting is
 * merged a number of times logarithmic in the runs, however many the memory budget makes.
 */
static void
spill(BuildState *bs)
{
	int			fanin = sift_merge_fanin(bs->budget);
	SiftRun    *run;

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
	while (tail_is_one_level(bs->runs, bs->nruns, fanin))
	{
		int			level = bs->runs[bs->nruns - 1]->level + 1;
		MemoryContext old = MemoryContextSwitchTo(bs->cxt);

		run = sift_merge_to_run(bs->runs + bs->nruns - fanin, fanin, bs->budget, &bs->home);
		MemoryContextSwitchTo(old);
		run->level = level;
		bs->nruns -= fanin - 1;
		bs->runs[bs->nruns - 1] = run;
	}
}

/* End of the runs from first on that go to one segment: their largest terms fit the limit together */
static int
group_end(SiftRun **runs, int first, int nruns, uint64 limit)
{
	int			last = first + 1;
	uint64		postings = sift_run_maxdf(runs[first]);

	while (last < nruns && postings + sift_run_maxdf(runs[last]) <= limit)
		postings += sift_run_maxdf(runs[last++]);
	return last;
}

static int
count_groups(SiftRun **runs, int nruns, uint64 limit)
{
	int			n = 0;

	for (int first = 0; first < nruns; first = group_end(runs, first, nruns, limit))
		n++;
	return n;
}

/* Writes the docs gathered since the last boundary as one or more segments */
static void
end_segment(BuildState *bs)
{
	int			first = 0;
	uint64		gen;
	SiftSegEntry *entry;
	/* a posting of the largest term costs about 10 bytes of the merge's memory */
	uint64		limit = TEST_KNOB(sift_test_segment_postings, bs->budget / 10);
	/* the entries the segments still to come (one per forced boundary, and the last) leave */
	int			room = SIFT_MAX_SEGMENTS - bs->nsegs - (bs->nbounds - bs->nextbound);

	if (bs->nruns == 0)
	{
		if (!sift_inv_empty(bs->inv))
		{
			entry = next_entry(bs, &gen);
			sift_inv_write(bs->inv, bs->wt, gen, &bs->ref, entry);
		}
		return;
	}
	if (!sift_inv_empty(bs->inv))
		spill(bs);
	/*
	 * Each segment merges runs whose largest terms fit the budget together. The directory holds a
	 * number of segments, though: a term is encoded whole, so when the budget would need more, fewer
	 * and larger segments are written, which take more memory than the budget in their largest terms.
	 */
	while (count_groups(bs->runs, bs->nruns, limit) > room)
		limit *= 2;
	while (first < bs->nruns)
	{
		int			last = group_end(bs->runs, first, bs->nruns, limit);

		entry = next_entry(bs, &gen);
		sift_runs_write(bs->runs + first, last - first, bs->wt, gen, &bs->ref, entry);
		first = last;
	}
	bs->nruns = 0;
}

static int
pagedoc_cmp(const void *a, const void *b)
{
	return ItemPointerCompare(&((PageDoc *) a)->tid, &((PageDoc *) b)->tid);
}

/* Analyzes the buffered docs of one heap page in tid order */
static void
flush_page(BuildState *bs)
{
	qsort(bs->docs, bs->ndocs, sizeof(PageDoc), pagedoc_cmp);
	for (int i = 0; i < bs->ndocs; i++)
	{
		SiftTermFreq *terms;
		uint32		dl;
		int			n = sift_analyze_doc(bs->an, bs->text.data + bs->docs[i].off, bs->docs[i].len, &terms, &dl);

		sift_inv_add(bs->inv, &bs->docs[i].tid, terms, n, dl);
		bs->indexed++;
		if (sift_inv_full(bs->inv))
			spill(bs);
	}
	bs->ndocs = 0;
	resetStringInfo(&bs->text);
	sift_analyzer_reset(bs->an);
	CHECK_FOR_INTERRUPTS();
}

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	BuildState *bs = state;
	MemoryContext old;
	text	   *t;
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
	if (bs->ndocs == bs->maxdocs)
	{
		bs->maxdocs *= 2;
		bs->docs = repalloc(bs->docs, bs->maxdocs * sizeof(PageDoc));
	}
	bs->docs[bs->ndocs++] = (PageDoc) {*tid, bs->text.len, VARSIZE_ANY_EXHDR(t)};
	appendBinaryStringInfo(&bs->text, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
	MemoryContextSwitchTo(old);
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
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);

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
	bs->ref = (SiftScoreRef) {(float4) opts->k1, (float4) opts->b, 0};
	bs->page = InvalidBlockNumber;
	bs->maxdocs = 64;
	bs->docs = palloc(bs->maxdocs * sizeof(PageDoc));
	initStringInfo(&bs->text);
	bs->maxruns = 16;
	bs->runs = palloc(bs->maxruns * sizeof(SiftRun *));
	MemoryContextSwitchTo(old);
	return bs;
}

/* Scans the heap, or this participant's share of a parallel scan, into the inverted buffer and its runs */
static double
build_scan(BuildState *bs, Relation heap, IndexInfo *info, TableScanDesc scan, bool progress)
{
	double		reltuples = table_index_build_scan(heap, bs->index, info, scan != NULL, progress, build_cb, bs, scan);

	flush_page(bs);
	return reltuples;
}

/* ---------- parallel ambuild ---------- */

/*
 * Every participant scans its share of a parallel heap scan (ascending keys,
 * interleaved with the others') into sorted runs. The workers' runs are files
 * of a shared fileset; the leader merges them with its own into the segments
 * a serial build would write.
 */
#define KEY_SHARED		UINT64CONST(0x5100000000000001)
#define KEY_PARTS		UINT64CONST(0x5100000000000002)
#define KEY_QUERY_TEXT	UINT64CONST(0x5100000000000003)
#define KEY_WAL_USAGE	UINT64CONST(0x5100000000000004)
#define KEY_BUFFER_USAGE UINT64CONST(0x5100000000000005)
#define SCAN_OF(shared)	((ParallelTableScanDesc) ((char *) (shared) + BUFFERALIGN(sizeof(SiftBuildShared))))

typedef struct SiftBuildShared
{
	Oid			heaprelid;
	Oid			indexrelid;
	bool		isconcurrent;
	int			nparticipants;
	Size		budget;				/* per participant */
	SharedFileSet fileset;
	slock_t		mutex;
	double		reltuples;
	double		indexed;
	/* the parallel table scan descriptor follows */
} SiftBuildShared;

/* A worker's runs, in the files sift_run_file(worker number, id) */
typedef struct SiftBuildPart
{
	int			nruns;
	struct
	{
		int			id;
		uint64		ndocs;
		uint64		maxdf;
	}			run[MAX_MERGE_FANIN];
} SiftBuildPart;

typedef struct SiftBuildLeader
{
	ParallelContext *pcxt;
	SiftBuildShared *shared;
	SiftBuildPart *parts;
	Snapshot	snapshot;
	WalUsage   *walusage;
	BufferUsage *bufferusage;
} SiftBuildLeader;

static void
end_parallel(SiftBuildLeader *ldr)
{
	DestroyParallelContext(ldr->pcxt);
	ExitParallelMode();
	if (IsMVCCSnapshot(ldr->snapshot))
		UnregisterSnapshot(ldr->snapshot);
}

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
static SiftBuildLeader *
begin_parallel(Relation heap, Relation index, IndexInfo *info)
{
	int			nworkers = info->ii_ParallelWorkers;
	SiftBuildLeader *ldr = palloc0(sizeof(SiftBuildLeader));
	SiftBuildShared *shared;
	ParallelContext *pcxt;
	Size		shared_size;
	int			nkeys = 4;

	ldr->snapshot = info->ii_Concurrent ? RegisterSnapshot(GetTransactionSnapshot()) : SnapshotAny;
	shared_size = add_size(BUFFERALIGN(sizeof(SiftBuildShared)), table_parallelscan_estimate(heap, ldr->snapshot));
	EnterParallelMode();
	ldr->pcxt = pcxt = CreateParallelContext("pg_sift", "sift_build_worker", nworkers);
	shm_toc_estimate_chunk(&pcxt->estimator, shared_size);
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(sizeof(SiftBuildPart), nworkers));
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(sizeof(WalUsage), nworkers));
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(sizeof(BufferUsage), nworkers));
	if (debug_query_string)
	{
		shm_toc_estimate_chunk(&pcxt->estimator, strlen(debug_query_string) + 1);
		nkeys++;
	}
	shm_toc_estimate_keys(&pcxt->estimator, nkeys);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		end_parallel(ldr);
		return NULL;
	}
	ldr->shared = shared = shm_toc_allocate(pcxt->toc, shared_size);
	*shared = (SiftBuildShared) {.heaprelid = RelationGetRelid(heap), .indexrelid = RelationGetRelid(index),
	.isconcurrent = info->ii_Concurrent,.nparticipants = nworkers + 1,
	.budget = (Size) maintenance_work_mem * 1024 / (nworkers + 1)};
	SharedFileSetInit(&shared->fileset, pcxt->seg);
	SpinLockInit(&shared->mutex);
	init_scan(heap, shared, ldr->snapshot);
	ldr->parts = shm_toc_allocate(pcxt->toc, mul_size(sizeof(SiftBuildPart), nworkers));
	ldr->walusage = shm_toc_allocate(pcxt->toc, mul_size(sizeof(WalUsage), nworkers));
	ldr->bufferusage = shm_toc_allocate(pcxt->toc, mul_size(sizeof(BufferUsage), nworkers));
	shm_toc_insert(pcxt->toc, KEY_SHARED, shared);
	shm_toc_insert(pcxt->toc, KEY_PARTS, ldr->parts);
	shm_toc_insert(pcxt->toc, KEY_WAL_USAGE, ldr->walusage);
	shm_toc_insert(pcxt->toc, KEY_BUFFER_USAGE, ldr->bufferusage);
	if (debug_query_string)
	{
		char	   *query = shm_toc_allocate(pcxt->toc, strlen(debug_query_string) + 1);

		strcpy(query, debug_query_string);
		shm_toc_insert(pcxt->toc, KEY_QUERY_TEXT, query);
	}
	LaunchParallelWorkers(pcxt);
	if (pcxt->nworkers_launched == 0)
	{
		end_parallel(ldr);
		return NULL;
	}
	WaitForParallelWorkersToAttach(pcxt);
	ereport(DEBUG1, errmsg("building sift index \"%s\" with %d parallel workers",
						   RelationGetRelationName(index), pcxt->nworkers_launched));
	return ldr;
}

/* Waits for the workers and adds their runs to the leader's; returns the tuple counts through res */
static void
adopt_runs(SiftBuildLeader *ldr, BuildState *bs, IndexBuildResult *res)
{
	SiftBuildShared *shared = ldr->shared;

	WaitForParallelWorkersToFinish(ldr->pcxt);
	for (int w = 0; w < ldr->pcxt->nworkers_launched; w++)
	{
		for (int i = 0; i < ldr->parts[w].nruns; i++)
		{
			SiftRun    *run = palloc(sizeof(SiftRun));

			if (bs->nruns == bs->maxruns)
			{
				bs->maxruns *= 2;
				bs->runs = repalloc(bs->runs, bs->maxruns * sizeof(SiftRun *));
			}
			*run = (SiftRun) {.file = BufFileOpenFileSet(&shared->fileset.fs, sift_run_file(w, ldr->parts[w].run[i].id), O_RDONLY, false),
				.ndocs = ldr->parts[w].run[i].ndocs,.maxdf = ldr->parts[w].run[i].maxdf,
			.fileset = &shared->fileset,.worker = w,.id = ldr->parts[w].run[i].id};
			bs->runs[bs->nruns++] = run;
		}
		InstrAccumParallelQuery(&ldr->bufferusage[w], &ldr->walusage[w]);
	}
	res->heap_tuples = shared->reltuples;
	res->index_tuples = shared->indexed;
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
	bs->nruns = sift_shrink_runs(bs->runs, bs->nruns, bs->budget, &bs->home);
	MemoryContextSwitchTo(old);
	part->nruns = bs->nruns;
	for (int i = 0; i < bs->nruns; i++)
	{
		part->run[i].id = bs->runs[i]->id;
		part->run[i].ndocs = bs->runs[i]->ndocs;
		part->run[i].maxdf = bs->runs[i]->maxdf;
		BufFileExportFileSet(bs->runs[i]->file);
		BufFileClose(bs->runs[i]->file);
	}
}

PGDLLEXPORT void sift_build_worker(dsm_segment *seg, shm_toc *toc);

void
sift_build_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftBuildShared *shared = shm_toc_lookup(toc, KEY_SHARED, false);
	SiftBuildPart *part = (SiftBuildPart *) shm_toc_lookup(toc, KEY_PARTS, false) + ParallelWorkerNumber;
	LOCKMODE	heaplock = shared->isconcurrent ? ShareUpdateExclusiveLock : ShareLock;
	LOCKMODE	indexlock = shared->isconcurrent ? RowExclusiveLock : AccessExclusiveLock;
	Relation	heap,
				index;
	IndexInfo  *info;
	BuildState *bs;
	double		reltuples;

	debug_query_string = shm_toc_lookup(toc, KEY_QUERY_TEXT, true);
	pgstat_report_activity(STATE_RUNNING, debug_query_string);
	heap = table_open(shared->heaprelid, heaplock);
	index = index_open(shared->indexrelid, indexlock);
	info = BuildIndexInfo(index);
	info->ii_Concurrent = shared->isconcurrent;
	SharedFileSetAttach(&shared->fileset, seg);
	InstrStartParallelQuery();
	bs = build_begin(index, shared->budget);
	bs->home = (RunHome) {.fileset = &shared->fileset,.worker = ParallelWorkerNumber};
	reltuples = build_scan(bs, heap, info, sift_table_beginscan_parallel(heap, SCAN_OF(shared)), false);
	hand_over_runs(bs, part);
	add_counts(shared, reltuples, bs->indexed);
	InstrEndParallelQuery(&((BufferUsage *) shm_toc_lookup(toc, KEY_BUFFER_USAGE, false))[ParallelWorkerNumber],
						  &((WalUsage *) shm_toc_lookup(toc, KEY_WAL_USAGE, false))[ParallelWorkerNumber]);
	MemoryContextDelete(bs->cxt);
	index_close(index, indexlock);
	table_close(heap, heaplock);
}

IndexBuildResult *
sift_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	const SiftOptions *opts = sift_index_options(index);
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));
	SiftBuildLeader *ldr = NULL;
	BuildState *bs;
	MemoryContext old;

	if (heap->rd_tableam != GetHeapamTableAmRoutine())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift indexes require a heap table")));
	if (opts->build_segments == 0 && info->ii_ParallelWorkers > 0)
		ldr = begin_parallel(heap, index, info);
	bs = build_begin(index, (Size) maintenance_work_mem * 1024 / (ldr ? ldr->shared->nparticipants : 1));
	old = MemoryContextSwitchTo(bs->cxt);
	if (ldr == NULL)
		init_bounds(bs, heap, opts->build_segments);
	bs->wt = sift_wt_begin(index, SIFT_WRITE_BULK);
	res->heap_tuples = build_scan(bs, heap, info, ldr ? sift_table_beginscan_parallel(heap, SCAN_OF(ldr->shared)) : NULL, true);
	res->index_tuples = bs->indexed;
	if (ldr)
	{
		add_counts(ldr->shared, res->heap_tuples, bs->indexed);
		adopt_runs(ldr, bs, res);
		bs->budget = (Size) maintenance_work_mem * 1024;
	}
	end_segment(bs);
	if (ldr)
		end_parallel(ldr);
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
