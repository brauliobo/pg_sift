/*
 * bm25/build/build.c - the PostgreSQL side of the build: heap scan, parallel workers and leader, ambuild and ambuildempty
 * The inverted buffer, the runs and their merge are shared with the other hosts (bm25/inv.c, bm25/runs/runs.c); the spill,
 * merge and segment writing of the build, and the page documents, are in build_runs.c.
 */
#include "bm25/build/int.h"

#include "datatype/timestamp.h"
#include "utils/timestamp.h"

#include "bm25/deadkeys.h"
#include "bm25/estimate.h"
#include "bm25/prefix.h"
#include "bm25/wmemo.h"
#include "core/format_ux.h"
#include "core/memtrack.h"

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

void
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

void
mem_watch(MemoryContext cxt)
{
	mem_root = cxt;
	test_merges = 0;
	sift_test_merge_term = test_merge_term;
	mem_root_cb.func = mem_root_gone;
	MemoryContextRegisterResetCallback(cxt, &mem_root_cb);
}
/* With sift_test_phase_gate set, entering a phase of the build waits for the advisory lock gate + phase (held by the test) */
void
sift_test_phase_wait(int phase)
{
	if (sift_test_phase_gate > 0)
		sift_test_hold(sift_test_phase_gate + phase);
}
#endif
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
void
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
void
progress_docs(BuildState *bs, bool all)
{
	if (bs->indexed - bs->reported < (all ? 1 : PROGRESS_DOCS))
		return;
	pgstat_progress_parallel_incr_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64) (bs->indexed - bs->reported));
	bs->reported = bs->indexed;
}
static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	BuildState *bs = state;
	MemoryContext old;
	text	   *t;
	bool		detoasted,
				keyed;
	uint64		key = 0;
	int			len;
	BlockNumber blk = ItemPointerGetBlockNumber(tid);

	old = MemoryContextSwitchTo(bs->tmpcxt);
	t = sift_bm25_assemble_doc(index, values, isnull, bs->tmpcxt);
	keyed = t != NULL && sift_bm25_unique_key(index, values, isnull, &key);
	MemoryContextSwitchTo(old);
	if (t == NULL)
		return;
	if (keyed && alive)
		sift_unique_build_add(bs->uniq, tid, key, values, isnull);
	if (blk != bs->page)
	{
		flush_page(bs);
		bs->page = blk;
		for (; bs->nextbound < bs->nbounds && blk >= bs->bounds[bs->nextbound]; bs->nextbound++)
			end_segment(bs);
	}
	old = MemoryContextSwitchTo(bs->tmpcxt);
	detoasted = (Pointer) t != DatumGetPointer(values[0]);
	len = VARSIZE_ANY_EXHDR(t);
	if (bs->text.len > 0 && bs->text.len + len > bs->page_text)
		flush_page(bs);
	if (len > bs->page_text)
	{
		check_order(bs, tid);
		analyze_doc(bs, tid, VARDATA_ANY(t), len, keyed ? &key : NULL);
		sift_analyzer_reset(bs->an);
	}
	else
	{
		if (bs->ndocs == bs->maxdocs)
		{
			bs->maxdocs *= 2;
			bs->docs = repalloc(bs->docs, bs->maxdocs * sizeof(PageDoc));
		}
		bs->docs[bs->ndocs++] = (PageDoc) {*tid, bs->text.len, len, keyed, key};
		if (bs->text.len + len < bs->text.maxlen)
		{
			memcpy(bs->text.data + bs->text.len, VARDATA_ANY(t), len);
			bs->text.len += len;
		}
		else
			appendBinaryStringInfo(&bs->text, VARDATA_ANY(t), len);
	}
	MemoryContextSwitchTo(old);
#ifdef SIFT_TESTING
	if (sift_test_page_text)
		mem_sample();
#endif
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
	BuildState *bs = palloc0(sizeof(BuildState));
	SiftAnalyzerConfig cfg;
	MemoryContext old;

	sift_analyzer_config_from_index(index, &cfg);
	bs->index = index;
	bs->analyzer_hash = cfg.hash;
	strlcpy(bs->analyzer_config, cfg.tsconfig, sizeof(bs->analyzer_config));
	bs->cxt = sift_mem_allocset_create(CurrentMemoryContext, "sift build");
	old = MemoryContextSwitchTo(bs->cxt);
	bs->tmpcxt = sift_mem_allocset_create(bs->cxt, "sift build tuple");
	bs->an = sift_analyzer_create(&cfg, bs->cxt);
	bs->positions = sift_index_options(index)->positions && !cfg.keyword;	/* not opts: the analyzer can look up a text search configuration, which reloads the entry */
	sift_analyzer_set_positions(bs->an, bs->positions);
	bs->budget = budget;
	bs->inv = sift_inv_create(bs->budget, bs->cxt, bs->positions);
	if (cfg.nfields)
		sift_inv_fields(bs->inv, cfg.nfields);
	bs->small_doc = sift_inv_small_doc(bs->inv);
	bs->page_text = page_text_max(bs);
	bs->ref = (SiftScoreRef) {(float4) sift_index_options(index)->k1, (float4) sift_index_options(index)->b, 0};
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
	MemSet(shared, 0, sizeof(SiftBuildShared));
	shared->common = (SiftParCommon) {RelationGetRelid(heap), RelationGetRelid(index), info->ii_Concurrent};
	shared->nparticipants = nworkers + 1;
	shared->budget = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, nworkers + 1, 1);
	ConditionVariableInit(&shared->ranges.cv);
	SharedFileSetInit(&shared->fileset, ldr->pcxt->seg);
	SpinLockInit(&shared->mutex);
	init_scan(heap, shared, ldr->snapshot);
	return sift_par_launch(ldr, "sift", index, nworkers) ? ldr : NULL;
}

/* Waits for the workers to hand their runs over and adds them to the leader's; returns the tuple counts through res */
static void
adopt_runs(SiftParLeader *ldr, BuildState *bs, IndexBuildResult *res)
{
	SiftBuildShared *shared = ldr->shared;
	SiftBuildPart *parts = ldr->parts;

	sift_ranges_wait_scanned(shared, ldr->pcxt->nworkers_launched);
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
			*run = (SiftRun) {.ndocs = parts[w].run[i].ndocs,.sum_dl = parts[w].run[i].sum_dl,.trailer = parts[w].run[i].trailer,
				.maxdf = parts[w].run[i].maxdf,.maxblob = parts[w].run[i].maxblob,.dict = parts[w].run[i].dict,
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

/* End of the runs from first on that one segment holds together, as end_segment groups them: what their largest terms and dictionaries add up to fits its limits */
static int
segment_group_end(SiftRun **runs, int first, int nruns, Size budget)
{
	uint64		df = runs[first]->maxdf,
				blob = runs[first]->maxblob,
				dict = runs[first]->dict;
	int			last = first + 1;

	for (; last < nruns; last++)
	{
		df += runs[last]->maxdf;
		blob += runs[last]->maxblob;
		dict += runs[last]->dict;
		if (df > sift_seg_postings(budget) || blob > SIFT_BLOB_MAX || dict > SIFT_DICT_MAX)
			break;
	}
	return last;
}

/*
 * Merges the runs of each segment group down to its share of target, so that no run holds what two segments would: the leader writes the same segments. Only more groups than
 * the slots of a participant hold (MAX_PART_RUNS, one run each at least) are merged across groups, down to those slots
 */
static int
shrink_by_group(SiftRun **runs, int nruns, int target, Size budget, RunHome *home)
{
	int			ngroups = 0,
				kept = 0;

	for (int first = 0; first < nruns; first = segment_group_end(runs, first, nruns, budget))
		ngroups++;
	for (int first = 0, last; first < nruns; first = last)
	{
		int			n;

		last = segment_group_end(runs, first, nruns, budget);
		n = sift_shrink_runs(runs + first, last - first, Max(1, target / ngroups), budget, home);
		memmove(runs + kept, runs + first, n * sizeof(SiftRun *));
		kept += n;
	}
	return kept > MAX_PART_RUNS ? sift_shrink_runs(runs, kept, MAX_PART_RUNS, budget, home) : kept;
}

/*
 * Spills what is left, merges the runs down to a share of what the leader merges at once (the participants together have its budget) and
 * makes them readable by the leader: the participants shrink their runs side by side, the leader would one after the other
 */
static void
hand_over_runs(BuildState *bs, SiftBuildPart *part, int nparticipants)
{
	MemoryContext old = MemoryContextSwitchTo(bs->cxt);
	int			target = Min(MAX_PART_RUNS, Max(1, sift_merge_fanin(bs->budget * nparticipants) / nparticipants));

	if (!sift_inv_empty(bs->inv))
		spill(bs);
	bs->nruns = shrink_by_group(bs->runs, bs->nruns, target, bs->budget, &bs->home);
	MemoryContextSwitchTo(old);
	part->nruns = bs->nruns;
	for (int i = 0; i < bs->nruns; i++)
	{
		part->run[i].id = bs->runs[i]->id;
		part->run[i].ndocs = bs->runs[i]->ndocs;
		part->run[i].sum_dl = bs->runs[i]->sum_dl;
		part->run[i].trailer = bs->runs[i]->trailer;
		part->run[i].maxdf = bs->runs[i]->maxdf;
		part->run[i].maxblob = bs->runs[i]->maxblob;
		part->run[i].dict = bs->runs[i]->dict;
		BufFileExportFileSet(bs->runs[i]->file);
		BufFileClose(bs->runs[i]->file);
	}
}

#define BUILD_WORKER_FLOOR	((Size) 4 << 20)

PGDLLEXPORT void sift_build_worker(dsm_segment *seg, shm_toc *toc);

void
sift_build_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;
	SiftBuildShared *shared;
	SiftBuildPart *part;
	BuildState *bs;
	double		reltuples;
	SiftMemTracker mt_bm25_build_worker;

	sift_par_worker_begin(&w, toc);
	shared = w.shared;
	part = (SiftBuildPart *) w.parts + ParallelWorkerNumber;
	SharedFileSetAttach(&shared->fileset, seg);
	bs = build_begin(w.index, shared->budget);
	bs->home = (RunHome) {.fileset = &shared->fileset,.worker = ParallelWorkerNumber};
	/* a worker's budget is a slice of the maintenance memory: the floor is what its buffers (the inverted buffer, the tuple and the runs) take at their smallest */
	sift_mem_init_sub(&mt_bm25_build_worker, "bm25_build_worker", bs->cxt, false, Max(shared->budget, BUILD_WORKER_FLOOR));
	reltuples = build_scan(bs, w.heap, w.info, sift_table_beginscan_parallel(w.heap, SCAN_OF(shared)), false);
	sift_mem_assert_within(&mt_bm25_build_worker);
	hand_over_runs(bs, part, shared->nparticipants);
	add_counts(shared, reltuples, bs->indexed);
#ifdef SIFT_TESTING
	MEM_SAMPLE();
	SpinLockAcquire(&shared->mutex);
	shared->peak_kb = Max(shared->peak_kb, sift_test_build_peak);
	SpinLockRelease(&shared->mutex);
#endif
	MemoryContextDelete(bs->cxt);
	sift_ranges_worker(shared, w.index);
	sift_par_worker_end(&w);
}

/* One line for a build that took a while or indexed many documents, none for the small and quick ones of a script or a test */
static void
say_built(Relation index, double docs, int nsegs, int64 us)
{
	char		count[SIFT_UX_QUANTITY],
				took[SIFT_UX_QUANTITY],
				size[SIFT_UX_QUANTITY];

	if (!sift_ux_should_emit_build(us, (uint64) docs, false, false))
		return;
	ereport(NOTICE,
			(errmsg("sift index \"%s\": built %s docs in %s (%s, %d segments, Format %u.%u)", RelationGetRelationName(index),
					sift_fmt_count(count, sizeof(count), (uint64) docs), sift_fmt_duration(took, sizeof(took), us),
					sift_fmt_bytes(size, sizeof(size), (uint64) RelationGetNumberOfBlocks(index) * BLCKSZ), nsegs, SIFT_FORMAT_MAJOR, SIFT_FORMAT_MINOR)));
}

/*
 * ON COMMIT DELETE ROWS, and a TRUNCATE of a table made in the transaction, build the index again in its truncated file, where the generations of the segments start
 * again: what the backend keeps of the file's segments (their bytes, unions, dead docs, memos and estimates) names the contents before and goes with them
 */
static void
forget_file(Relation index)
{
	sift_segcache_drop_file(&index->rd_locator);
	sift_prefix_drop_file(&index->rd_locator);
	sift_deadkeys_drop_file(&index->rd_locator);
	sift_wide_memo_drop_file(&index->rd_locator);
	sift_estimate_drop_file(&index->rd_locator);
}

IndexBuildResult *
sift_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	const SiftOptions *opts = sift_index_options(index);
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));
	SiftParLeader *ldr = NULL;
	BuildState *bs;
	MemoryContext old;
	TimestampTz began = GetCurrentTimestamp();

	if (heap->rd_tableam != GetHeapamTableAmRoutine() && !sift_tbl_is_table(heap))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift indexes require a heap or sift_table table"),
				 errhint("Index a heap table, or copy the rows into one with CREATE TABLE ... AS SELECT.")));
	forget_file(index);
	if (opts->build_segments == 0 && info->ii_ParallelWorkers > 0 && !index->rd_index->indisunique)
	{
		SiftAnalyzerConfig cfg;

		/* the options are refused here: a worker that refuses them first reports the error to the leader with the context of the worker */
		sift_analyzer_config_from_index(index, &cfg);
		ldr = begin_parallel(heap, index, info);
	}
	bs = build_begin(index, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, ldr ? ((SiftBuildShared *) ldr->shared)->nparticipants : 1, 1));
	old = MemoryContextSwitchTo(bs->cxt);
	SiftMemTracker mt_bm25_build;

	if (index->rd_index->indisunique)
		bs->uniq = sift_unique_build_begin(heap, index, bs->cxt);

	sift_mem_init_sub(&mt_bm25_build, "bm25_build", bs->cxt, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, ldr ? ((SiftBuildShared *) ldr->shared)->nparticipants : 1, 1));
	if (ldr == NULL)
		init_bounds(bs, heap, sift_index_options(index)->build_segments);
	else
	{
		/* the leader's runs are files of the fileset too: the participants read them to write the terms */
		bs->shared = ldr->shared;
		bs->home = (RunHome) {.fileset = &bs->shared->fileset,.worker = ldr->pcxt->nworkers_launched};
	}
	bs->wt = sift_wt_begin(index, SIFT_WRITE_BULK);
	if (heap->rd_rel->reltuples > 0)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
	progress_phase(bs, PHASE_SCAN);
	res->heap_tuples = build_scan(bs, heap, info, ldr ? sift_table_beginscan_parallel(heap, SCAN_OF(ldr->shared)) : NULL, true);
	res->index_tuples = bs->indexed;
	if (bs->uniq)
		sift_unique_build_end(bs->uniq);
	if (ldr)
	{
		add_counts(ldr->shared, res->heap_tuples, bs->indexed);
		adopt_runs(ldr, bs, res);
		bs->budget = sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1);
	}
	bs->scanned = true;
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) res->index_tuples);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, 0);
	end_segment(bs);
	sift_mem_assert_within(&mt_bm25_build);
	if (ldr)
	{
		sift_ranges_close(bs->shared);
		sift_par_wait(ldr);
		sift_par_end(ldr);
	}
	sift_store_init_meta(bs->wt, sift_index_options(index), bs->analyzer_hash, bs->analyzer_config);
	sift_wt_finish(bs->wt);
	if (bs->nsegs > 0)
		sift_dir_update(index, &(SiftDirChange) {.fn = publish,.arg = bs,.nadd = bs->nsegs,.extra = InvalidBuffer});
	say_built(index, res->index_tuples, bs->nsegs, GetCurrentTimestamp() - began);
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
	sift_store_init_meta(wt, sift_index_options(index), cfg.hash, cfg.tsconfig);
	sift_wt_finish(wt);
}
