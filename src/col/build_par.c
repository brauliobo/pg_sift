/*
 * col/build_par.c - parallel CREATE INDEX / REINDEX of a sift_col index (docs/COL.md 2.6). The unit of work is a superblock: every
 * participant (the workers and the leader) claims the next one from a shared counter, scans its 256 heap pages, forms the batch and
 * encodes the unit into segments of its own, written through a SHARED write target after the leader has written the metapage. The
 * leader publishes the segments of all the participants in one directory update. A superblock is in exactly one segment.
 */
#include "postgres.h"

#include "access/parallel.h"
#include "access/table.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/condition_variable.h"
#include "tcop/tcopprot.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "col/build.h"
#include "sift_parbuild.h"

/* The directory is shared by the participants: 16 of them leave 7 segments each */
#define MAX_WORKERS		15
#define MIN_BUDGET		(16 * 1024 * 1024)

typedef struct ColShared
{
	SiftParCommon common;
	BlockNumber nblocks;		/* of the heap when the build started */
	uint32		nsbs;			/* superblocks of those */
	Size		budget;			/* of the segment writer of a participant */
	int			maxsegs;		/* segments of a participant */
	int			nslots;			/* participants planned (the leader included): the size of the parts */
	int			nlaunched;		/* workers launched, set before ready */
	pg_atomic_uint32 ready;		/* the metapage is written */
	ConditionVariable readycv;
	pg_atomic_uint32 next;		/* the next superblock nobody has claimed */
	pg_atomic_uint64 done;		/* rows indexed by all */
	slock_t		mutex;
	double		reltuples;
	double		indexed;
} ColShared;

/* The segments of a participant: slot i is worker i, the last launched one the leader's */
typedef struct ColPart
{
	int			nsegs;
	SiftSegEntry segs[FLEXIBLE_ARRAY_MEMBER];
} ColPart;

static Size
part_size(int maxsegs)
{
	return MAXALIGN(offsetof(ColPart, segs) + maxsegs * sizeof(SiftSegEntry));
}

static ColPart *
part_of(const ColShared *sh, char *parts, int slot)
{
	return (ColPart *) (parts + slot * part_size(sh->maxsegs));
}

static void
add_counts(ColShared *sh, double reltuples, double indexed)
{
	SpinLockAcquire(&sh->mutex);
	sh->reltuples += reltuples;
	sh->indexed += indexed;
	SpinLockRelease(&sh->mutex);
}

/*
 * A participant claims whole superblocks, the first one by its slot so that every participant has a share whenever the table has
 * enough, whatever the speed of the workers' start.
 */
static void
participate(ColShared *sh, char *parts, int slot, Relation heap, Relation index, IndexInfo *info, Snapshot snapshot)
{
	ColPart    *part = part_of(sh, parts, slot);
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	ColBuild   *b = sift_colb_begin(index, wt, sh->budget, sh->maxsegs, part->segs, &sh->done, slot == sh->nlaunched);
	double		reltuples = 0;

	for (uint32 sb = slot; sb < sh->nsbs; sb = pg_atomic_fetch_add_u32(&sh->next, 1))
	{
#ifdef SIFT_TESTING
		if (sift_test_col_fail_sb == sb + 1)
			elog(ERROR, "sift test: the participant of superblock %u fails", sb);
#endif
		reltuples += sift_colb_scan_sb(b, heap, info, snapshot, sb, sh->nblocks);
	}
	part->nsegs = sift_colb_end(b);
	sift_wt_finish(wt);
	add_counts(sh, reltuples, sift_colb_rows(b));
	sift_colb_free(b);
}

/* The workers need the metapage and the superblocks to share before they allocate a block */
static void
open_gate(ColShared *sh, Relation index, int nlaunched)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_BULK);

	sift_store_init_meta_ex(wt, 0, 0, sift_col_rel(index)->schema->hash, SIFT_COL_META_FLAG);
	sift_wt_finish(wt);
	sh->nlaunched = nlaunched;
	pg_atomic_write_u32(&sh->next, nlaunched + 1);
	pg_atomic_write_membarrier_u32(&sh->ready, 1);
	ConditionVariableBroadcast(&sh->readycv);
}

static void
await_gate(ColShared *sh)
{
	ConditionVariablePrepareToSleep(&sh->readycv);
	while (pg_atomic_read_membarrier_u32(&sh->ready) == 0)
		ConditionVariableSleep(&sh->readycv, PG_WAIT_EXTENSION);
	ConditionVariableCancelSleep();
}

static void
init_shared(ColShared *sh, Relation heap, Relation index, IndexInfo *info, int nslots)
{
	*sh = (ColShared) {.common = {RelationGetRelid(heap), RelationGetRelid(index), info->ii_Concurrent},
		.nblocks = RelationGetNumberOfBlocks(heap), .nslots = nslots,
		.budget = Max((Size) maintenance_work_mem * 1024 / 2 / nslots, MIN_BUDGET), .maxsegs = SIFT_COL_BUILD_SEGMENTS / nslots};
	sh->nsbs = ((uint64) sh->nblocks + SIFT_SB_PAGES - 1) >> SIFT_SB_SHIFT;
	pg_atomic_init_u32(&sh->ready, 0);
	pg_atomic_init_u32(&sh->next, 0);
	pg_atomic_init_u64(&sh->done, 0);
	ConditionVariableInit(&sh->readycv);
	SpinLockInit(&sh->mutex);
}

/* NULL when no worker could be launched: nothing is written and the caller builds alone */
static SiftParLeader *
begin_parallel(Relation heap, Relation index, IndexInfo *info)
{
	int			nworkers = Min(info->ii_ParallelWorkers, MAX_WORKERS);
	SiftParLeader *ldr = sift_par_enter(info->ii_Concurrent);
	ColShared  *sh;

	if (!sift_par_setup(ldr, "sift_col_build_worker", nworkers, sizeof(ColShared),
						mul_size(part_size(SIFT_COL_BUILD_SEGMENTS / (nworkers + 1)), nworkers + 1), info->ii_Concurrent))
		return NULL;
	sh = ldr->shared;
	init_shared(sh, heap, index, info, nworkers + 1);
	if (!sift_par_launch(ldr, "sift_col", index, nworkers))
		return NULL;
	open_gate(sh, index, ldr->pcxt->nworkers_launched);
	return ldr;
}

/* The segments of every participant that wrote one */
static SiftSegEntry *
collect_segments(const SiftParLeader *ldr, int *nsegs)
{
	const ColShared *sh = ldr->shared;
	SiftSegEntry *segs = palloc(SIFT_COL_BUILD_SEGMENTS * sizeof(SiftSegEntry));

	*nsegs = 0;
	for (int slot = 0; slot <= sh->nlaunched; slot++)
	{
		ColPart    *part = part_of(sh, ldr->parts, slot);

		memcpy(segs + *nsegs, part->segs, part->nsegs * sizeof(SiftSegEntry));
		*nsegs += part->nsegs;
	}
	return segs;
}

IndexBuildResult *
sift_colp_build(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *res;
	SiftParLeader *ldr = begin_parallel(heap, index, info);
	ColShared  *sh;
	SiftSegEntry *segs;
	int			nsegs;

	if (ldr == NULL)
		return NULL;
	sh = ldr->shared;
	res = palloc0(sizeof(IndexBuildResult));
	if (heap->rd_rel->reltuples > 0)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_SCAN);
	participate(sh, ldr->parts, sh->nlaunched, heap, index, info, ldr->snapshot);
	sift_par_wait(ldr);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_WRITE);
	res->heap_tuples = sh->reltuples;
	res->index_tuples = sh->indexed;
#ifdef SIFT_TESTING
	sift_test_col_workers = sh->nlaunched;
#endif
	segs = collect_segments(ldr, &nsegs);
	sift_par_end(ldr);
	sift_colb_publish(index, segs, nsegs);
	return res;
}

PGDLLEXPORT void sift_col_build_worker(dsm_segment *seg, shm_toc *toc);

void
sift_col_build_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;

	sift_par_worker_begin(&w, toc);
	await_gate(w.shared);
	participate(w.shared, w.parts, ParallelWorkerNumber, w.heap, w.index, w.info, w.snapshot);
	sift_par_worker_end(&w);
}
