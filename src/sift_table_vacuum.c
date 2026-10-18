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

#include "sift_table.h"

typedef struct DeadTids
{
	ItemPointerData *tid;
	uint64		n;
	uint64		cap;
} DeadTids;

static void
dead_add(DeadTids *d, BlockNumber vblock, OffsetNumber off)
{
	if (d->n == d->cap)
	{
		d->cap = Max(1024, d->cap * 2);
		d->tid = d->tid ? repalloc_huge(d->tid, d->cap * sizeof(ItemPointerData)) :
			palloc_extended(d->cap * sizeof(ItemPointerData), MCXT_ALLOC_HUGE);
	}
	ItemPointerSet(&d->tid[d->n++], vblock, off);
}

static int
cmp_tid(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) a, (ItemPointer) b);
}

static bool
dead_reaped(ItemPointer itemptr, void *state)
{
	DeadTids   *d = state;

	return bsearch(itemptr, d->tid, d->n, sizeof(ItemPointerData), cmp_tid) != NULL;
}

/* Is xid's transaction over and aborted */
static bool
xid_aborted(TransactionId xid)
{
	return !TransactionIdIsCurrentTransactionId(xid) && !TransactionIdIsInProgress(xid) && !TransactionIdDidCommit(xid);
}

typedef struct VacState
{
	TransactionId oldest;
	TransactionId newest_kept;	/* oldest xid still stored: relfrozenxid */
	double		live;
	double		dead;			/* recently dead: deleter committed but not removable yet */
	DeadTids	dead_tids;
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

	if (xid_aborted(e->xid))
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
			if (TransactionIdIsNormal(rec->xmin) && xid_aborted(rec->xmin))
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
				dead_add(&v->dead_tids, ph->vblock, off);
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

	(void) vacuum_get_cutoffs(rel, params, &cutoffs);
	v.oldest = cutoffs.OldestXmin;
	v.newest_kept = cutoffs.OldestXmin;
	sift_tbl_refresh(rel, st);
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
		qsort(v.dead_tids.tid, v.dead_tids.n, sizeof(ItemPointerData), cmp_tid);
	}
	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &irel);
	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo = {.index = irel[i],.heaprel = rel,.analyze_only = false,.report_progress = false,
		.estimated_count = false,.message_level = DEBUG2,.num_heap_tuples = v.live,.strategy = bstrategy};
		IndexBulkDeleteResult *res = NULL;

		if (v.dead_tids.n > 0)
			res = index_bulk_delete(&ivinfo, NULL, dead_reaped, &v.dead_tids);
		res = index_vacuum_cleanup(&ivinfo, res);
		if (res)
			pfree(res);
	}
	vac_close_indexes(nindexes, irel, NoLock);
	if (st->initialized)
		kept = sift_tbl_tomb_rewrite(rel, st, tomb_keep, &v);
	(void) kept;
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

void
sift_tbl_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin, TransactionId *xid_cutoff, MultiXactId *multi_cutoff,
								   double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
	TupleTableSlot *slot = table_slot_create(OldTable, NULL);
	TupleDesc	desc = RelationGetDescr(OldTable);
	SiftTblRel *old_st = sift_tbl_rel(OldTable),
			   *new_st = sift_tbl_rel(NewTable);
	Snapshot	snap = SnapshotAny;
	TableScanDesc scan = NULL;
	IndexScanDesc iscan = NULL;
	Datum	   *values = palloc(desc->natts * sizeof(Datum));
	bool	   *isnull = palloc(desc->natts * sizeof(bool));

	*num_tuples = *tups_vacuumed = *tups_recently_dead = 0;
	sift_tbl_ensure_init(NewTable);
	if (OldIndex != NULL)
	{
#if PG_VERSION_NUM >= 180000
		iscan = index_beginscan(OldTable, OldIndex, SnapshotAny, NULL, 0, 0);
#else
		iscan = index_beginscan(OldTable, OldIndex, SnapshotAny, 0, 0);
#endif
		index_rescan(iscan, NULL, 0, NULL, 0);
	}
	else
		scan = table_beginscan(OldTable, snap, 0, NULL);
	sift_tbl_refresh(OldTable, old_st);
	sift_tbl_tomb_sync(OldTable, old_st);
	for (;;)
	{
		SiftTblRowVer rv;
		SiftTblDel	del;
		HeapTuple	tup;
		MinimalTuple mt;

		if (iscan ? !index_getnext_slot(iscan, ForwardScanDirection, slot) : !table_scan_getnextslot(scan, ForwardScanDirection, slot))
			break;
		CHECK_FOR_INTERRUPTS();
		if (!sift_tbl_fetch_row(OldTable, old_st, &slot->tts_tid, &rv))
			continue;
		heap_freetuple(rv.tuple);
		if ((rv.flags & SIFT_TBL_REC_DEAD) || (TransactionIdIsNormal(rv.xmin) && xid_aborted(rv.xmin)))
		{
			*tups_vacuumed += 1;
			continue;
		}
		sift_tbl_del_state(old_st, rv.key, &del);
		if (del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED)
		{
			if (TransactionIdPrecedes(del.xid, OldestXmin))
				*tups_vacuumed += 1;
			else
				*tups_recently_dead += 1;
			continue;
		}
		slot_getallattrs(slot);
		memcpy(values, slot->tts_values, desc->natts * sizeof(Datum));
		memcpy(isnull, slot->tts_isnull, desc->natts * sizeof(bool));
		tup = heap_form_tuple(desc, values, isnull);
		mt = sift_tbl_minimal_from_heap_tuple(tup);
		{
			ItemPointerData tid;

			sift_tbl_append(NewTable, new_st, mt, TransactionIdIsCurrentTransactionId(rv.xmin) ? rv.xmin : FrozenTransactionId,
							rv.cmin, 0, &tid);
		}
		pfree(mt);
		heap_freetuple(tup);
		*num_tuples += 1;
	}
	if (iscan)
		index_endscan(iscan);
	else
		table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
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
	ItemPointerData *have = NULL;
	uint64		nhave = 0,
				cap = 0;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	econtext->ecxt_scantuple = slot;
	indexInfo->ii_PredicateState = ExecPrepareQual(indexInfo->ii_Predicate, estate);
	/* the TIDs the first pass found in the index, sorted (every one of them is looked up below) */
	while (tuplesort_getdatum(state->tuplesort, true, false, &ts_val, &ts_isnull, NULL))
	{
		if (nhave == cap)
		{
			cap = Max(1024, cap * 2);
			have = have ? repalloc_huge(have, cap * sizeof(ItemPointerData)) :
				palloc_extended(cap * sizeof(ItemPointerData), MCXT_ALLOC_HUGE);
		}
		itemptr_decode(&have[nhave++], DatumGetInt64(ts_val));
		state->itups += 1;
	}
	scan = table_beginscan(table_rel, snapshot, 0, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		ItemPointerData tid = slot->tts_tid;

		CHECK_FOR_INTERRUPTS();
		state->htups += 1;
		if (nhave > 0 && bsearch(&tid, have, nhave, sizeof(ItemPointerData), cmp_tid) != NULL)
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
	if (have)
		pfree(have);
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
