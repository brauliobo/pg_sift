/*
 * col/v1/build/btree_par.c - the parallel build of a key_cut index through a btree on the key
 * The leaf level of the btree is split at keys of the first column into ranges that hold every row of a value, and the participants (the workers and the leader)
 * take the ranges one after the other, each scanning its range in the order of the btree with a read stream of its own (as many reads in flight as
 * maintenance_io_concurrency allows) and writing the runs of the rows through its own segment writer. Two ranges do not meet in the key, so the runs do not overlap
 * and nothing is merged or spooled: the leader publishes the segments of every participant. The ranges number their units from a base each (the entries before the
 * range, over the least rows of a unit, and room for the runs' last units), so the units of the ranges follow each other as the ranges do.
 */
#include "postgres.h"
#include "access/parallel.h"
#include "access/tableam.h"
#include "commands/progress.h"
#include "pgstat.h"
#include "storage/condition_variable.h"
#include "utils/datum.h"
#include "utils/rel.h"
#include "col/v1/build/int.h"
#include "core/memtrack.h"
#include "core/parbuild.h"
#include "core/waits.h"

#define MAX_PARTICIPANTS	(SIFT_COL_BTREE_WORKERS + 1)
#define RANGES_PER_PARTICIPANT	4
#define MAX_RANGES			(MAX_PARTICIPANTS * RANGES_PER_PARTICIPANT + 2)
#define UNIT_ROWS_MIN		32			/* the fewest rows of a unit but the last of a run */
#define UNIT_SLACK			1024		/* the last units of the runs of a range */

typedef struct BtShared
{
	SiftParCommon common;
	Oid			btree;
	Size		budget;
	int			segcap;
	int			nslots;			/* participants planned: the memory and the directory are shared by them */
	int			nlaunched;
	int			nranges;
	int			nbound;			/* the keys the ranges are cut at: range i is [bound[i - 1], bound[i]), the last but one has no end, the last holds the NULLs */
	uint32		unit_base[MAX_RANGES + 1];
	pg_atomic_uint32 ready;
	ConditionVariable readycv;
	pg_atomic_uint32 next;		/* the next range nobody has taken */
	ColScanAcc	scan;
	slock_t		mutex;
	double		reltuples;
	double		indexed;
	char		keys[FLEXIBLE_ARRAY_MEMBER];	/* the keys, datumSerialize'd */
} BtShared;

/* The boundaries of the ranges, evenly among the samples of the walk, and the units each range starts at */
static Size
init_ranges(BtShared *sh, const BtPlan *plan, const FormData_pg_attribute *att, int pieces)
{
	char	   *p = sh->keys;
	int			nb = Min(plan->nbound, pieces - 1);

	sh->nbound = nb;
	sh->nranges = nb + 2;
	for (int k = 0; k < nb; k++)
	{
		const BtBound *bd = &plan->bound[(k + 1) * plan->nbound / (nb + 1)];

		datumSerialize(bd->key, false, att->attbyval, att->attlen, &p);
		sh->unit_base[k + 1] = (uint32) (bd->before / UNIT_ROWS_MIN) + (k + 1) * UNIT_SLACK;
	}
	sh->unit_base[0] = 0;
	sh->unit_base[nb + 1] = (uint32) (plan->entries / UNIT_ROWS_MIN) + (nb + 1) * UNIT_SLACK;
	sh->unit_base[nb + 2] = 0;
	return p - sh->keys;
}

/* Range i of the shared state */
static BtRange
range_of(const BtShared *sh, int i)
{
	BtRange		rg = {0};
	const char *p = sh->keys;
	bool		isnull;

	rg.nulls = i == sh->nbound + 1;
	rg.has_lo = !rg.nulls && i >= 1;
	rg.has_hi = !rg.nulls && i < sh->nbound;
	for (int k = 0; k <= i && k < sh->nbound; k++)
	{
		Datum		key = datumRestore((char **) &p, &isnull);

		if (k == i - 1)
			rg.lo = key;
		if (k == i)
			rg.hi = key;
	}
	rg.unit_base = sh->unit_base[i];
	rg.unit_limit = i < sh->nbound + 1 ? sh->unit_base[i + 1] : 0;
	return rg;
}

static void
participate(BtShared *sh, char *parts, int slot, Relation heap, Relation index, IndexInfo *info, Snapshot snapshot)
{
	SiftColPart *part = sift_colp_part(parts, sh->segcap, slot);
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	ColBuild   *b = sift_colb_begin(index, wt, sh->budget, part->segs, sh->segcap, true, sh->nslots, false);
	double		reltuples = 0;
	uint32		i;

	SiftMemTracker mt_col_build;

	sift_mem_init_sub(&mt_col_build, "col_build", b->cxt, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, sh->nslots, 1));
	PG_TRY();
	{
		sift_colb_scan_watch(&sh->scan);
		while ((i = pg_atomic_fetch_add_u32(&sh->next, 1)) < sh->nranges)
		{
			BtRange		rg = range_of(sh, i);

			reltuples += colb_btree_scan(b, heap, info, snapshot, sh->btree, &rg);
			sift_mem_assert_within(&mt_col_build);
		}
		part->nsegs = sift_colb_end(b);
	}
	PG_FINALLY();
	{
		sift_colb_scan_watch(NULL);
	}
	PG_END_TRY();
	sift_wt_finish(wt);
	SpinLockAcquire(&sh->mutex);
	sh->reltuples += reltuples;
	sh->indexed += sift_colb_rows(b);
	SpinLockRelease(&sh->mutex);
	sift_colb_free(b);
}

static void
open_gate(BtShared *sh, Relation index, int nlaunched)
{
	sift_colb_init_meta(index);
	sh->nlaunched = nlaunched;
	pg_atomic_write_membarrier_u32(&sh->ready, 1);
	ConditionVariableBroadcast(&sh->readycv);
}

static void
await_gate(BtShared *sh)
{
	ConditionVariablePrepareToSleep(&sh->readycv);
	while (pg_atomic_read_membarrier_u32(&sh->ready) == 0)
		ConditionVariableSleep(&sh->readycv, sift_wait_event(SIFT_WAIT_BUILD_GATE));
	ConditionVariableCancelSleep();
}

/* The index, or NULL when no worker could be launched or the btree gave too few keys to split at: nothing is written then and the caller builds alone */
IndexBuildResult *
colb_btree_build_par(Relation heap, Relation index, IndexInfo *info, const BtPlan *plan, int nworkers)
{
	const FormData_pg_attribute *att;
	Relation	bt = index_open(plan->btree, AccessShareLock);
	SiftParLeader *ldr;
	BtShared   *sh;
	IndexBuildResult *res;
	SiftSegEntry *segs;
	Size		keybytes = 0;
	Snapshot	snapshot;
	int			segcap,
				nsegs,
				pieces = (nworkers + 1) * RANGES_PER_PARTICIPANT;

	att = TupleDescAttr(RelationGetDescr(bt), 0);
	index_close(bt, NoLock);
	if (plan->nbound < 2)
		return NULL;
	for (int i = 0; i < plan->nbound; i++)
		keybytes += datumEstimateSpace(plan->bound[i].key, false, att->attbyval, att->attlen);
	segcap = sift_colb_segs_bound(index, ((uint64) RelationGetNumberOfBlocks(heap) + SIFT_SB_PAGES - 1) >> SIFT_SB_SHIFT);
	snapshot = info->ii_Concurrent ? NULL : RegisterSnapshot(GetLatestSnapshot());
	ldr = sift_par_enter(info->ii_Concurrent);
	if (snapshot != NULL)
		ldr->snapshot = snapshot;
	if (!sift_par_setup(ldr, "sift_col_btree_worker", nworkers, offsetof(BtShared, keys) + keybytes,
						mul_size(sift_colp_part_size(segcap), nworkers + 1), true))
		return NULL;
	sh = ldr->shared;
	*sh = (BtShared) {.common = {RelationGetRelid(heap), RelationGetRelid(index), info->ii_Concurrent}, .btree = plan->btree,
		.budget = sift_colb_budget(nworkers + 1), .segcap = segcap, .nslots = nworkers + 1};
	pg_atomic_init_u32(&sh->ready, 0);
	pg_atomic_init_u32(&sh->next, 0);
	SpinLockInit(&sh->scan.mutex);
	SpinLockInit(&sh->mutex);
	ConditionVariableInit(&sh->readycv);
	(void) init_ranges(sh, plan, att, pieces);
	info->ii_BrokenHotChain = true;
	if (!sift_par_launch(ldr, "sift_col", index, nworkers))
		return NULL;
	open_gate(sh, index, ldr->pcxt->nworkers_launched);
	res = palloc0(sizeof(IndexBuildResult));
	if (heap->rd_rel->reltuples > 0)
		pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64) heap->rd_rel->reltuples);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_SCAN);
	participate(sh, ldr->parts, sh->nlaunched, heap, index, info, ldr->snapshot);
	sift_par_wait(ldr);
	sift_colb_scan_report(index, &sh->scan);
	pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, SIFT_COL_PHASE_WRITE);
	res->heap_tuples = sh->reltuples;
	res->index_tuples = sh->indexed;
#ifdef SIFT_TESTING
	sift_test_col_workers = sh->nlaunched;
#endif
	segs = sift_colp_collect(ldr->parts, sh->segcap, sh->nlaunched + 1, &nsegs);
	sift_par_end(ldr);
	sift_colb_publish(index, segs, nsegs, !info->ii_Concurrent && sift_col_track_on(index));
	return res;
}

PGDLLEXPORT void sift_col_btree_worker(dsm_segment *seg, shm_toc *toc);

void
sift_col_btree_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;

	sift_par_worker_begin(&w, toc);
	await_gate(w.shared);
	participate(w.shared, w.parts, ParallelWorkerNumber, w.heap, w.index, w.info, w.snapshot);
	sift_par_worker_end(&w);
}
