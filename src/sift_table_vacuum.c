/*
 * sift_table_vacuum.c - VACUUM, VACUUM FULL / CLUSTER (rewrite), CREATE INDEX CONCURRENTLY validation (docs/COL-TAM.md section 7).
 *
 * VACUUM: delta rows whose insertion aborted, or whose deleter is committed and older than OldestXmin, are flagged DEAD (and are
 * the dead TIDs the indexes drop); committed old insertions are frozen; then the tombstone segment is rewritten without the
 * entries that no longer matter. TIDs are never reused, so no ordering between these steps is needed for safety.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"
#include "utils/tuplesort.h"

#include "sift_maint.h"
#include "sift_table.h"

#define MAX_MERGE_STEPS	16

/* A growable array of TIDs: the dead rows VACUUM found, the index entries CREATE INDEX CONCURRENTLY already has (sorted to look up) */
typedef struct TidList
{
	ItemPointerData *tid;
	uint64		n;
	uint64		cap;
} TidList;

/* The next free slot */
static ItemPointer
tid_push(TidList *d)
{
	if (d->n == d->cap)
	{
		d->cap = Max(1024, d->cap * 2);
		d->tid = d->tid ? repalloc_huge(d->tid, d->cap * sizeof(ItemPointerData)) :
			palloc_extended(d->cap * sizeof(ItemPointerData), MCXT_ALLOC_HUGE);
	}
	return &d->tid[d->n++];
}

static int
cmp_tid(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

static void
tid_sort(TidList *d)
{
	qsort(d->tid, d->n, sizeof(ItemPointerData), cmp_tid);
}

/* In the sorted list? */
static bool
tid_has(ItemPointer itemptr, void *state)
{
	TidList    *d = state;

	return d->n > 0 && bsearch(itemptr, d->tid, d->n, sizeof(ItemPointerData), cmp_tid) != NULL;
}

typedef struct VacState
{
	TransactionId oldest;
	TransactionId newest_kept;	/* oldest xid still stored: relfrozenxid */
	double		live;
	double		dead;			/* recently dead: deleter committed but not removable yet */
	TidList	dead_tids;
} VacState;

static void
note_xid(VacState *v, TransactionId xid)
{
	if (TransactionIdIsNormal(xid) && TransactionIdPrecedes(xid, v->newest_kept))
		v->newest_kept = xid;
}

/* Is the tombstone entry removable now: aborted, a lock of a finished transaction, or a delete whose row VACUUM has just flagged */
static bool
tomb_keep(const SiftTblTomb *e, void *arg)
{
	VacState   *v = arg;

	if (sift_tbl_xid_aborted(e->xid))
		return false;
	if (e->kind == SIFT_TBL_TOMB_LOCK)
	{
		if (TransactionIdIsInProgress(e->xid) || TransactionIdIsCurrentTransactionId(e->xid))
		{
			note_xid(v, e->xid);
			return true;
		}
		return false;
	}
	if (TransactionIdDidCommit(e->xid) && TransactionIdPrecedes(e->xid, v->oldest))
		return false;
	note_xid(v, e->xid);
	return true;
}

/* Vacuums one delta page: flags dead rows, freezes old insertions; one WAL record */
static void
vacuum_page(Relation rel, SiftTblRel *st, const SiftTblSeg *seg, uint32 idx, VacState *v)
{
	Buffer		buf = sift_tbl_lock_page(rel, seg, idx);
	GenericXLogState *state = GenericXLogStart(rel);
	Page		page = GenericXLogRegisterBuffer(state, buf, 0);
	SiftTblPageHdr *ph = TBL_PAGE(page);
	bool		changed = false;

	if (!(ph->flags & SIFT_TBL_PAGE_OVFDATA))
		for (uint32 off = 1; off <= ph->nrows; off++)
		{
			SiftTblRec *rec;
			SiftTblDel	del;
			uint64		key = ((uint64) ph->vblock << 16) | off;
			bool		dead = false;

			if (ph->slot[off - 1] == 0)
				continue;
			rec = (SiftTblRec *) (SiftPageGetData(page) + ph->slot[off - 1]);
			if (rec->flags & SIFT_TBL_REC_DEAD)
				continue;
			if (TransactionIdIsNormal(rec->xmin) && sift_tbl_xid_aborted(rec->xmin))
				dead = true;
			else
			{
				sift_tbl_del_state(st, key, &del);
				if ((del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED) &&
					TransactionIdPrecedes(del.xid, v->oldest))
					dead = true;
				else if (del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED)
					v->dead += 1;
				else
					v->live += 1;
			}
			if (dead)
			{
				rec->flags |= SIFT_TBL_REC_DEAD;
				changed = true;
				ItemPointerSet(tid_push(&v->dead_tids), ph->vblock, off);
				continue;
			}
			if (TransactionIdIsNormal(rec->xmin) && !TransactionIdIsCurrentTransactionId(rec->xmin) &&
				!TransactionIdIsInProgress(rec->xmin) && TransactionIdPrecedes(rec->xmin, v->oldest))
			{
				rec->xmin = FrozenTransactionId;
				changed = true;
			}
			note_xid(v, rec->xmin);
		}
	if (changed)
		GenericXLogFinish(state);
	else
		GenericXLogAbort(state);
	UnlockReleaseBuffer(buf);
}

/* Is the vblock in a delta segment (whose rows vacuum_page judges) */
static bool
in_delta(const SiftTblRel *st, uint64 vblock)
{
	for (int i = 0; i < st->ndelta; i++)
		if (vblock >= st->delta[i].vbase && vblock < st->delta[i].vbase + st->delta[i].vcap)
			return true;
	return false;
}

/* The dead TIDs the tombstones say for rows of the column groups: committed deletions older than OldestXmin; [*lo, *hi] their superblocks */
static void
group_dead(const SiftTblRel *st, VacState *v, const SiftTblTomb *a, uint32 n, uint32 *lo, uint32 *hi)
{
	for (uint32 i = 0; i < n; i++)
	{
		const SiftTblTomb *e = &a[i];
		uint32		sb = (e->key >> 16) / SIFT_SB_PAGES;

		if (e->kind == SIFT_TBL_TOMB_LOCK || !TransactionIdDidCommit(e->xid) || !TransactionIdPrecedes(e->xid, v->oldest) ||
			in_delta(st, e->key >> 16))
			continue;
		sift_key_to_tid(e->key, tid_push(&v->dead_tids));
		*lo = Min(*lo, sb);
		*hi = Max(*hi, sb);
	}
}

/* Closes the active delta segment once it is worth a column segment (pages) */
#define VACUUM_CLOSE_PAGES	8

void
sift_tbl_relation_vacuum(Relation rel, struct VacuumParams *params, BufferAccessStrategy bstrategy)
{
	VacState	v = {0};
	struct VacuumCutoffs cutoffs;
	SiftTblRel *st = sift_tbl_rel(rel);
#if PG_VERSION_NUM >= 180000
	TimestampTz start = GetCurrentTimestamp();
#endif
	Relation   *irel;
	int			nindexes;
	BlockNumber nblocks;
	uint64		kept = 0;
	uint32		sb_lo = PG_UINT32_MAX,
				sb_hi = 0;

	(void) vacuum_get_cutoffs(rel, params, &cutoffs);
	v.oldest = cutoffs.OldestXmin;
	v.newest_kept = cutoffs.OldestXmin;
	sift_tbl_refresh(rel, st);
	sift_maint_lock(rel, true);
	if (st->initialized)
	{
		sift_tbl_tomb_sync(rel, st);
		for (int i = 0; i < st->ndelta; i++)
		{
			SiftTblSeg seg = st->delta[i];

			sift_tbl_seg_load(rel, &seg);
			for (uint32 p = 0; p < seg.npages; p++)
			{
				CHECK_FOR_INTERRUPTS();
				vacuum_page(rel, st, &seg, p, &v);
			}
		}
		group_dead(st, &v, st->ents, st->nents, &sb_lo, &sb_hi);
		group_dead(st, &v, st->recent, st->nrecent, &sb_lo, &sb_hi);
		tid_sort(&v.dead_tids);
		if (sb_lo <= sb_hi)
			(void) sift_tbl_group_bulkdelete(rel, st, tid_has, &v.dead_tids, sb_lo, sb_hi);
	}
	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &irel);
	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo = {.index = irel[i],.heaprel = rel,.analyze_only = false,.report_progress = false,
		.estimated_count = false,.message_level = DEBUG2,.num_heap_tuples = v.live,.strategy = bstrategy};
		IndexBulkDeleteResult *res = NULL;

		if (v.dead_tids.n > 0)
			res = index_bulk_delete(&ivinfo, NULL, tid_has, &v.dead_tids);
		res = index_vacuum_cleanup(&ivinfo, res);
		if (res)
			pfree(res);
	}
	vac_close_indexes(nindexes, irel, NoLock);
	if (st->initialized)
	{
		sift_tbl_close_delta(rel, st, VACUUM_CLOSE_PAGES);
		(void) sift_tbl_seal(rel, st, v.oldest, true);
		for (int i = 0; i < MAX_MERGE_STEPS && sift_tbl_merge(rel, st); i++)
			CHECK_FOR_INTERRUPTS();
		kept = sift_tbl_tomb_rewrite(rel, st, tomb_keep, &v);
		(void) sift_recycle(rel);
		(void) sift_maint_orphan_sweep(rel);
	}
	(void) kept;
	sift_maint_unlock(rel);
	nblocks = RelationGetNumberOfBlocks(rel);
	{
		bool		fz,
					mz;

#if PG_VERSION_NUM >= 180000
		vac_update_relstats(rel, nblocks, v.live, 0, 0, nindexes > 0, v.newest_kept, cutoffs.OldestMxact, &fz, &mz, false);
#else
		vac_update_relstats(rel, nblocks, v.live, 0, nindexes > 0, v.newest_kept, cutoffs.OldestMxact, &fz, &mz, false);
#endif
	}
#if PG_VERSION_NUM >= 180000
	pgstat_report_vacuum(RelationGetRelid(rel), rel->rd_rel->relisshared, (PgStat_Counter) v.live,
						 (PgStat_Counter) v.dead, start);
#else
	pgstat_report_vacuum(RelationGetRelid(rel), rel->rd_rel->relisshared, (PgStat_Counter) v.live, (PgStat_Counter) v.dead);
#endif
}

/* ---------- rewrite: VACUUM FULL, CLUSTER, ALTER TABLE ---------- */

/* Copies one stored version into the new table unless it is dead; rv->tuple is freed */
static void
copy_version(SiftTblRel *old_st, Relation NewTable, SiftTblRel *new_st, TransactionId OldestXmin, SiftTblRowVer *rv, double *num_tuples,
			 double *tups_vacuumed, double *tups_recently_dead)
{
	SiftTblDel	del;
	MinimalTuple mt;
	ItemPointerData tid;

	if ((rv->flags & SIFT_TBL_REC_DEAD) || (TransactionIdIsNormal(rv->xmin) && sift_tbl_xid_aborted(rv->xmin)))
		*tups_vacuumed += 1;
	else if (sift_tbl_del_state(old_st, rv->key, &del), del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED)
	{
		if (TransactionIdPrecedes(del.xid, OldestXmin))
			*tups_vacuumed += 1;
		else
			*tups_recently_dead += 1;
	}
	else
	{
		mt = sift_tbl_minimal_from_heap_tuple(rv->tuple);
		sift_tbl_append(NewTable, new_st, mt, TransactionIdIsCurrentTransactionId(rv->xmin) ? rv->xmin : FrozenTransactionId,
						rv->cmin, 0, &tid);
		pfree(mt);
		*num_tuples += 1;
	}
	heap_freetuple(rv->tuple);
}

void
sift_tbl_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff,
								   double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
	SiftTblRel *old_st = sift_tbl_rel(OldTable),
			   *new_st = sift_tbl_rel(NewTable);
	SiftTblRowVer rv;

	*num_tuples = *tups_vacuumed = *tups_recently_dead = 0;
	sift_tbl_ensure_init(NewTable);
	sift_tbl_refresh(OldTable, old_st);
	sift_tbl_tomb_sync(OldTable, old_st);
	if (OldIndex != NULL)
	{
		TupleTableSlot *slot = table_slot_create(OldTable, NULL);
#if PG_VERSION_NUM >= 180000
		IndexScanDesc iscan = index_beginscan(OldTable, OldIndex, SnapshotAny, NULL, 0, 0);
#else
		IndexScanDesc iscan = index_beginscan(OldTable, OldIndex, SnapshotAny, 0, 0);
#endif

		index_rescan(iscan, NULL, 0, NULL, 0);
		while (index_getnext_slot(iscan, ForwardScanDirection, slot))
		{
			CHECK_FOR_INTERRUPTS();
			if (sift_tbl_fetch_row(OldTable, old_st, &slot->tts_tid, &rv))
				copy_version(old_st, NewTable, new_st, OldestXmin, &rv, num_tuples, tups_vacuumed, tups_recently_dead);
		}
		index_endscan(iscan);
		ExecDropSingleTupleTableSlot(slot);
	}
	else
	{
		TableScanDesc scan = table_beginscan(OldTable, SnapshotAny, 0, NULL);

		while (sift_tbl_scan_next_stored(scan, &rv))
		{
			CHECK_FOR_INTERRUPTS();
			copy_version(old_st, NewTable, new_st, OldestXmin, &rv, num_tuples, tups_vacuumed, tups_recently_dead);
		}
		table_endscan(scan);
	}
	/* the copy is final: its rows become columns now */
	sift_tbl_close_delta(NewTable, new_st, 1);
	(void) sift_tbl_seal(NewTable, new_st, OldestXmin, true);
	*xid_cutoff = OldestXmin;
	*multi_cutoff = GetOldestMultiXactId();
}

/* ---------- CREATE INDEX CONCURRENTLY: second pass ---------- */

void
sift_tbl_index_validate_scan(Relation table_rel, Relation index_rel, IndexInfo *indexInfo, Snapshot snapshot,
							 ValidateIndexState *state)
{
	EState	   *estate = CreateExecutorState();
	ExprContext *econtext = GetPerTupleExprContext(estate);
	TupleTableSlot *slot = table_slot_create(table_rel, NULL);
	TableScanDesc scan;
	Datum		ts_val;
	bool		ts_isnull;
	TidList		have = {0};
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	econtext->ecxt_scantuple = slot;
	indexInfo->ii_PredicateState = ExecPrepareQual(indexInfo->ii_Predicate, estate);
	/* the TIDs the first pass found in the index, sorted (every one of them is looked up below) */
	while (tuplesort_getdatum(state->tuplesort, true, false, &ts_val, &ts_isnull, NULL))
	{
		itemptr_decode(tid_push(&have), DatumGetInt64(ts_val));
		state->itups += 1;
	}
	scan = table_beginscan(table_rel, snapshot, 0, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		ItemPointerData tid = slot->tts_tid;

		CHECK_FOR_INTERRUPTS();
		state->htups += 1;
		if (tid_has(&tid, &have))
			continue;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (indexInfo->ii_PredicateState != NULL && !ExecQual(indexInfo->ii_PredicateState, econtext))
			continue;
		FormIndexDatum(indexInfo, slot, estate, values, isnull);
		index_insert(index_rel, values, isnull, &tid, table_rel,
					 indexInfo->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO, false, indexInfo);
		state->tups_inserted += 1;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	indexInfo->ii_PredicateState = NULL;
	if (have.tid)
		pfree(have.tid);
}

/* ---------- TABLESAMPLE ---------- */

bool
sift_tbl_scan_sample_next_block(TableScanDesc scan, struct SampleScanState *scanstate)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("TABLESAMPLE is not supported for sift_table tables")));
	return false;
}

bool
sift_tbl_scan_sample_next_tuple(TableScanDesc scan, struct SampleScanState *scanstate, TupleTableSlot *slot)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("TABLESAMPLE is not supported for sift_table tables")));
	return false;
}
