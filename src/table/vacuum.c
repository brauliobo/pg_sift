/*
 * table/vacuum.c - VACUUM, VACUUM FULL / CLUSTER (rewrite), CREATE INDEX CONCURRENTLY validation (docs/COL-TAM.md section 7).
 *
 * VACUUM: write buffer rows whose insertion aborted, or whose deleter is committed and older than OldestXmin, are flagged DEAD (and are
 * the dead TIDs the indexes drop); committed old insertions are frozen; then the tombstone segment is rewritten without the
 * entries that no longer matter. TIDs are never reused, so no ordering between these steps is needed for safety. Column segments
 * left mostly dead are rewritten, and the free run at the end of the file goes back to the operating system.
 *
 * VACUUM FULL / CLUSTER write the kept versions straight into column segments of the new table (sift_tbl_bulk_*).
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
#include "catalog/pg_am_d.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"

#include "col/seg.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "table/table.h"

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
	double		live;			/* as a heap counts them: in-progress insertions left out, in-progress deletions in */
	double		dead;			/* recently dead: deleter committed but not removable yet */
	double		group_dead;		/* of those, rows of the column groups (their liveness bit is still set) */
	TidList	dead_tids;
	SiftTblKeyRuns dead_runs;	/* sorted: the rows of the groups with unfrozen rows whose inserter aborted */
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

/* Vacuums one write buffer page: flags dead rows, freezes old insertions; one WAL record */
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
				else if (!TransactionIdIsNormal(rec->xmin) || !TransactionIdIsInProgress(rec->xmin))
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

/* Is the vblock in a write buffer (whose rows vacuum_page judges) */
static bool
in_wbuf(const SiftTblRel *st, uint64 vblock)
{
	for (int i = 0; i < st->nwbuf; i++)
		if (vblock >= st->wbuf[i].vbase && vblock < st->wbuf[i].vbase + st->wbuf[i].vcap)
			return true;
	return false;
}

/*
 * The dead TIDs the tombstones say for rows of the column groups: committed deletions older than OldestXmin; [*lo, *hi] their
 * superblocks. Committed deletions not old enough yet are counted as recently dead. A row has one committed deletion at most.
 */
static void
group_dead(const SiftTblRel *st, VacState *v, const SiftTblTomb *a, uint32 n, uint32 *lo, uint32 *hi)
{
	for (uint32 i = 0; i < n; i++)
	{
		const SiftTblTomb *e = &a[i];
		uint32		sb = (e->key >> 16) / SIFT_SB_PAGES;

		if (e->kind == SIFT_TBL_TOMB_LOCK || in_wbuf(st, e->key >> 16) || !TransactionIdDidCommit(e->xid))
			continue;
		if (!TransactionIdPrecedes(e->xid, v->oldest))
		{
			v->dead += 1;
			v->group_dead += 1;
			continue;
		}
		sift_key_to_tid(e->key, tid_push(&v->dead_tids));
		*lo = Min(*lo, sb);
		*hi = Max(*hi, sb);
	}
}

static int
cmp_run(const void *a, const void *b)
{
	SiftKey		x = ((const SiftKey *) a)[0],
				y = ((const SiftKey *) b)[0];

	return x < y ? -1 : x > y;
}

/* Is the row dead for the indexes: a dead TID, or a row of an aborted writer in a group (a key in one of the runs) */
static bool
dead_has(ItemPointer itemptr, void *state)
{
	VacState   *v = state;
	SiftKey		key = sift_key_from_tid(itemptr);
	int			lo = 0,
				hi = v->dead_runs.n;

	if (tid_has(itemptr, &v->dead_tids))
		return true;
	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (v->dead_runs.run[mid][0] <= key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo > 0 && key <= v->dead_runs.run[lo - 1][1];
}

/* The writers of the groups with unfrozen rows hold the oldest xid back */
static void
note_groups(const SiftTblRel *st, VacState *v)
{
	for (int i = 0; i < st->dir.nsegs; i++)
		if (sift_tbl_seg_unfrozen(&st->dir.segs[i]))
			note_xid(v, st->dir.segs[i].reserved);
}

/* Steps 1 and 2: flags the dead write buffer rows, clears the liveness of the dead group rows, counts the rows */
static void
vacuum_rows(Relation rel, SiftTblRel *st, VacState *v)
{
	uint32		sb_lo = PG_UINT32_MAX,
				sb_hi = 0;

	sift_tbl_tomb_sync(rel, st);
	for (int i = 0; i < st->nwbuf; i++)
	{
		SiftTblSeg	seg = st->wbuf[i];

		sift_tbl_seg_load(rel, &seg);
		for (uint32 p = 0; p < seg.npages; p++)
		{
			CHECK_FOR_INTERRUPTS();
			vacuum_page(rel, st, &seg, p, v);
		}
	}
	group_dead(st, v, st->ents, st->nents, &sb_lo, &sb_hi);
	group_dead(st, v, st->recent, st->nrecent, &sb_lo, &sb_hi);
	v->dead_runs.mcxt = CurrentMemoryContext;
	sift_tbl_aborted_runs(rel, st, &v->dead_runs, &sb_lo, &sb_hi);
	tid_sort(&v->dead_tids);
	if (v->dead_runs.n > 0)
		qsort(v->dead_runs.run, v->dead_runs.n, sizeof(SiftKey[2]), cmp_run);
	if (sb_lo <= sb_hi)
		(void) sift_tbl_group_bulkdelete(rel, st, dead_has, v, sb_lo, sb_hi);
	/* the bulk delete flagged the segments dirty: their liveness is read from entries that say so */
	sift_tbl_refresh(rel, st);
	v->live += sift_tbl_group_live_rows(rel, st) - v->group_dead;
}

/* Step 3; returns the number of indexes */
static int
vacuum_indexes(Relation rel, VacState *v, BufferAccessStrategy bstrategy)
{
	Relation   *irel;
	int			nindexes;

	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &irel);
	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo = {.index = irel[i],.heaprel = rel,.analyze_only = false,.report_progress = false,
		.estimated_count = false,.message_level = DEBUG2,.num_heap_tuples = v->live + v->dead,.strategy = bstrategy};
		IndexBulkDeleteResult *res = NULL;

		if (v->dead_tids.n > 0 || v->dead_runs.n > 0)
			res = index_bulk_delete(&ivinfo, NULL, dead_has, v);
		res = index_vacuum_cleanup(&ivinfo, res);
		if (res)
			pfree(res);
	}
	vac_close_indexes(nindexes, irel, NoLock);
	return nindexes;
}

/* Would the tombstone rewrite drop an entry? tomb_keep notes the xids of the entries kept either way */
static bool
tomb_drops(SiftTblRel *st, VacState *v)
{
	bool		drops = false;

	for (uint32 i = 0; i < st->nents; i++)
		drops |= !tomb_keep(&st->ents[i], v);
	for (uint32 i = 0; i < st->nrecent; i++)
		drops |= !tomb_keep(&st->recent[i], v);
	return drops;
}

/* The table buffer adds a virtual-to-physical page vector and overflow links to the common lane layout. */
static void
relocate_page(Page page, const SiftExtent *old, const SiftExtent *fresh, int n)
{
	if (SiftPageGetOpaque(page)->type == SIFT_PT_WBUFHEADER)
	{
		SiftTblWbufHdr *h = TBL_WBUF_HDR(page);

		for (uint32 i = 0; i < h->npages; i++)
			h->page[i] = sift_store_relocated_block(h->page[i], old, fresh, n);
	}
	else if (SiftPageGetOpaque(page)->type == SIFT_PT_WBUFDATA)
	{
		SiftTblPageHdr *h = TBL_PAGE(page);

		if (h->flags & SIFT_TBL_PAGE_OVFDATA)
			h->ovf_next = sift_store_relocated_block(h->ovf_next, old, fresh, n);
		else
			for (uint32 i = 1; i <= h->nrows; i++)
			{
				SiftTblRec *rec = (SiftTblRec *) sift_tbl_page_rec(page, SiftPageGetOpaque(page)->gen, i);

				if (rec && (rec->flags & SIFT_TBL_REC_OVF))
					rec->aux = sift_store_relocated_block(rec->aux, old, fresh, n);
			}
	}
}

/* Closes the open write buffer segment once it is worth a column segment (pages) */
#define VACUUM_CLOSE_PAGES	8

/*
 * Steps 4 and 5, then the space: the blocks released before (by earlier VACUUMs and writers) are recycled, and the end of the file is
 * given back (sift_maint_space): under AccessExclusiveLock, which keeps every reader out, what this VACUUM released is reusable at once
 * (a lazy VACUUM's own xid is in no snapshot or horizon, so outside that lock it is reused once another transaction has ended), so the
 * segments that end the file are written again into the free blocks below them and the cut finds the end free.
 */
static void
vacuum_space(Relation rel, SiftTblRel *st, VacState *v, bool truncate, int elevel)
{
	(void) sift_tbl_drop_dead_groups(rel, st);
	sift_tbl_close_wbuf(rel, st, VACUUM_CLOSE_PAGES);
	(void) sift_tbl_promote(rel, st, v->oldest, true);
	(void) sift_tbl_freeze_groups(rel, st, v->oldest);
	for (int i = 0; i < MAX_MERGE_STEPS && sift_tbl_merge(rel, st, v->oldest); i++)
		CHECK_FOR_INTERRUPTS();
	if (tomb_drops(st, v))
		(void) sift_tbl_tomb_rewrite(rel, st, tomb_keep, v);
	(void) sift_recycle(rel);
	(void) sift_maint_orphan_sweep(rel);
	if (truncate)
		sift_maint_space(rel, relocate_page, elevel);
}

/* Step 6, with the counts of a heap: reltuples and the live rows, the recently dead ones for autovacuum */
static void
vacuum_report(Relation rel, const VacState *v, const struct VacuumCutoffs *cutoffs, bool hasindex, TimestampTz start, int elevel)
{
	BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
	bool		fz,
				mz;

#if PG_VERSION_NUM >= 180000
	vac_update_relstats(rel, nblocks, v->live, 0, 0, hasindex, v->newest_kept, cutoffs->OldestMxact, &fz, &mz, false);
#else
	vac_update_relstats(rel, nblocks, v->live, 0, hasindex, v->newest_kept, cutoffs->OldestMxact, &fz, &mz, false);
#endif
	sift_pgstat_report_vacuum(rel, (PgStat_Counter) v->live, (PgStat_Counter) v->dead, start);
	ereport(elevel, (errmsg("table \"%s\": rows: %llu removed, %.0f remain, %.0f are dead but not yet removable",
							RelationGetRelationName(rel), (unsigned long long) v->dead_tids.n, v->live, v->dead)));
}

void
sift_tbl_relation_vacuum(Relation rel, SIFT_TAM_VACUUM_PARAMS, BufferAccessStrategy bstrategy)
{
	VacState	v = {0};
	struct VacuumCutoffs cutoffs;
	SiftTblRel *st = sift_tbl_rel(rel);
	TimestampTz start = GetCurrentTimestamp();
	int			elevel = (params->options & VACOPT_VERBOSE) ? INFO : DEBUG2;
	int			nindexes;

	(void) vacuum_get_cutoffs(rel, params, &cutoffs);
	v.oldest = cutoffs.OldestXmin;
	v.newest_kept = cutoffs.OldestXmin;
	sift_tbl_refresh(rel, st);
	sift_maint_lock(rel, true);
	if (st->initialized)
		vacuum_rows(rel, st, &v);
	nindexes = vacuum_indexes(rel, &v, bstrategy);
	if (st->initialized)
	{
		vacuum_space(rel, st, &v, params->truncate == VACOPTVALUE_ENABLED, elevel);
		note_groups(st, &v);
	}
	sift_maint_unlock(rel);
	vacuum_report(rel, &v, &cutoffs, nindexes > 0, start, elevel);
}

/* ---------- rewrite: VACUUM FULL, CLUSTER ---------- */

typedef struct CopyState
{
	SiftTblRel *st;
	TransactionId oldest;
	double	   *num_tuples;
	double	   *tups_vacuumed;
	double	   *tups_recently_dead;
} CopyState;

/*
 * Does the rewrite keep the version: inserted (committed, or by this transaction) and not deleted. A deletion by this transaction is
 * final for the new table: the rewrite commits or aborts with it, and the table is locked against every other.
 */
static bool
copy_keep(const SiftTblRowVer *rv, void *arg)
{
	CopyState  *c = arg;
	SiftTblDel	del;

	if ((rv->flags & SIFT_TBL_REC_DEAD) || (TransactionIdIsNormal(rv->xmin) && sift_tbl_xid_aborted(rv->xmin)))
		*c->tups_vacuumed += 1;
	else if (sift_tbl_del_state(c->st, rv->key, &del), del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED)
		*(TransactionIdPrecedes(del.xid, c->oldest) ? c->tups_vacuumed : c->tups_recently_dead) += 1;
	else if (del.state == SIFT_TBL_DEL_SELF)
		*c->tups_recently_dead += 1;
	else
	{
		*c->num_tuples += 1;
		return true;
	}
	return false;
}

/* CLUSTER by an index scan (an index that cannot order a sort): the versions in the order of OldIndex, each fetched once */
static void
copy_index_order(Relation OldTable, Relation OldIndex, SiftTblBulk *bulk, CopyState *c)
{
	IndexScanDesc iscan = sift_index_beginscan(OldTable, OldIndex, SnapshotAny, 0);
	ItemPointer tid;
	SiftTblRowVer rv;

	index_rescan(iscan, NULL, 0, NULL, 0);
	while ((tid = index_getnext_tid(iscan, ForwardScanDirection)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		if (!sift_tbl_fetch_row(OldTable, c->st, tid, &rv))
			continue;
		if (copy_keep(&rv, c))
			sift_tbl_bulk_tuple(bulk, rv.tuple);
		sift_tbl_free_row(&rv);
	}
	index_endscan(iscan);
}

/* The versions as a scan returns them, as tuples, into the new table or into sort: column segments of another layout are read by attribute */
static void
copy_scan(Relation OldTable, SiftTblBulk *bulk, Tuplesortstate *sort, CopyState *c)
{
	TableScanDesc scan = sift_table_beginscan(OldTable, SnapshotAny, 0, NULL);
	SiftTblRowVer rv;

	while (sift_tbl_scan_next_stored(scan, &rv))
	{
		CHECK_FOR_INTERRUPTS();
		if (copy_keep(&rv, c))
		{
			if (sort)
				tuplesort_putheaptuple(sort, rv.tuple);
			else
				sift_tbl_bulk_tuple(bulk, rv.tuple);
		}
		heap_freetuple(rv.tuple);
	}
	table_endscan(scan);
}

/* CLUSTER by a sort in the order of a btree index */
static void
copy_sorted(Relation OldTable, Relation OldIndex, SiftTblBulk *bulk, CopyState *c)
{
	Tuplesortstate *sort = tuplesort_begin_cluster(RelationGetDescr(OldTable), OldIndex, maintenance_work_mem, NULL, TUPLESORT_NONE);
	HeapTuple	tup;

	copy_scan(OldTable, NULL, sort, c);
	tuplesort_performsort(sort);
	while ((tup = tuplesort_getheaptuple(sort, true)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		sift_tbl_bulk_tuple(bulk, tup);
	}
	tuplesort_end(sort);
}

/*
 * The kept versions are written into the new table as frozen column segments with dense TIDs: in the order of the index for CLUSTER,
 * else the units of the column groups re-keyed as they decode and then the write buffer rows. Recently dead versions are dropped (no snapshot
 * of the new table can want them). A TID fetch decodes the unit of its superblock, so CLUSTER sorts whenever the index can order a
 * sort (btree), whatever the planner's heap costs chose (use_sort): an index scan in random order would decode a unit per row.
 */
void
sift_tbl_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort,
								   TransactionId OldestXmin, SIFT_TAM_CLUSTER_SNAPSHOT TransactionId *xid_cutoff, MultiXactId *multi_cutoff,
								   double *num_tuples, double *tups_vacuumed, double *tups_recently_dead)
{
	SiftTblRel *new_st = sift_tbl_rel(NewTable);
	CopyState	c = {sift_tbl_rel(OldTable), OldestXmin, num_tuples, tups_vacuumed, tups_recently_dead};
	SiftTblBulk *bulk;

	*num_tuples = *tups_vacuumed = *tups_recently_dead = 0;
	new_st->fresh = true;
	sift_tbl_layout_adopt(NewTable, new_st, OldTable);
	sift_tbl_ensure_init(NewTable);
	sift_tbl_refresh(OldTable, c.st);
	sift_tbl_tomb_sync(OldTable, c.st);
	bulk = sift_tbl_bulk_begin(NewTable, new_st);
	if (OldIndex != NULL && OldIndex->rd_rel->relam == BTREE_AM_OID)
		copy_sorted(OldTable, OldIndex, bulk, &c);
	else if (OldIndex != NULL)
		copy_index_order(OldTable, OldIndex, bulk, &c);
	else if (!sift_tbl_bulk_copy(bulk, OldTable, c.st, copy_keep, &c))
		copy_scan(OldTable, bulk, NULL, &c);
	sift_tbl_bulk_finish(bulk);
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
	scan = sift_table_beginscan(table_rel, snapshot, 0, NULL);
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
