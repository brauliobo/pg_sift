/*
 * table/v1/vacuum_copy.c - VACUUM FULL / CLUSTER (rewrite into column segments of the new table) and CREATE INDEX CONCURRENTLY validation.
 */
#include "vacuum_int.h"

#include "access/tupdesc.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

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

/* Where the versions of a rewrite go: the new table, or a sort in front of it */
typedef void (*TupleSink) (void *arg, HeapTuple tup);

static void
put_bulk(void *arg, HeapTuple tup)
{
	sift_tbl_bulk_tuple(arg, tup);
}

static void
put_cluster(void *arg, HeapTuple tup)
{
	tuplesort_putheaptuple(arg, tup);
}

typedef struct KeySort
{
	Tuplesortstate *sort;
	TupleTableSlot *slot;
} KeySort;

static void
put_key(void *arg, HeapTuple tup)
{
	KeySort    *k = arg;

	ExecForceStoreHeapTuple(tup, k->slot, false);
	tuplesort_puttupleslot(k->sort, k->slot);
}

/* The versions as a scan returns them, as tuples, into the new table or into a sort: column segments of another layout are read by attribute */
static void
copy_scan(Relation OldTable, TupleSink put, void *arg, CopyState *c)
{
	TableScanDesc scan = sift_table_beginscan(OldTable, SnapshotAny, 0, NULL);
	SiftTblRowVer rv;

	while (sift_tbl_scan_next_stored(scan, &rv))
	{
		CHECK_FOR_INTERRUPTS();
		if (copy_keep(&rv, c))
			put(arg, rv.tuple);
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

	copy_scan(OldTable, put_cluster, sort, c);
	tuplesort_performsort(sort);
	while ((tup = tuplesort_getheaptuple(sort, true)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		sift_tbl_bulk_tuple(bulk, tup);
	}
	tuplesort_end(sort);
}

/* The table's key_order: the kept versions sorted by its columns (default btree order, nulls last) before they get their dense TIDs */
static void
copy_key_sorted(Relation OldTable, SiftTblBulk *bulk, CopyState *c, int nkeys, const AttrNumber *att)
{
	TupleDesc	desc = RelationGetDescr(OldTable);
	Oid			ops[SIFT_TBL_KEY_MAX],
				colls[SIFT_TBL_KEY_MAX];
	bool		nulls_first[SIFT_TBL_KEY_MAX] = {false};
	KeySort		k;
	TupleTableSlot *out;

	for (int i = 0; i < nkeys; i++)
	{
		Form_pg_attribute a = TupleDescAttr(desc, att[i] - 1);

		ops[i] = lookup_type_cache(a->atttypid, TYPECACHE_LT_OPR)->lt_opr;
		colls[i] = a->attcollation;
	}
	k.sort = tuplesort_begin_heap(desc, nkeys, (AttrNumber *) att, ops, colls, nulls_first, maintenance_work_mem, NULL, TUPLESORT_NONE);
	k.slot = MakeSingleTupleTableSlot(desc, &TTSOpsHeapTuple);
	copy_scan(OldTable, put_key, &k, c);
	ExecDropSingleTupleTableSlot(k.slot);
	tuplesort_performsort(k.sort);
	out = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	while (tuplesort_gettupleslot(k.sort, true, false, out, NULL))
	{
		bool		shouldfree;
		HeapTuple	tup = ExecFetchSlotHeapTuple(out, false, &shouldfree);

		CHECK_FOR_INTERRUPTS();
		sift_tbl_bulk_tuple(bulk, tup);
		if (shouldfree)
			heap_freetuple(tup);
	}
	ExecDropSingleTupleTableSlot(out);
	tuplesort_end(k.sort);
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
	AttrNumber	att[SIFT_TBL_KEY_MAX];
	int			nkeys;

	*num_tuples = *tups_vacuumed = *tups_recently_dead = 0;
	new_st->fresh = true;
	sift_tbl_layout_adopt(NewTable, new_st, OldTable);
	sift_tbl_ensure_init(NewTable);
	sift_tbl_refresh(OldTable, c.st);
	sift_tomb_sync_all(OldTable, &c.st->tomb);
	bulk = sift_tbl_bulk_begin(NewTable, new_st);
	if (OldIndex != NULL && OldIndex->rd_rel->relam == BTREE_AM_OID)
		copy_sorted(OldTable, OldIndex, bulk, &c);
	else if (OldIndex != NULL)
		copy_index_order(OldTable, OldIndex, bulk, &c);
	else if ((nkeys = sift_tbl_key_order(OldTable, c.st, att)) > 0)
		copy_key_sorted(OldTable, bulk, &c, nkeys, att);
	else if (!sift_tbl_bulk_copy(bulk, OldTable, c.st, copy_keep, &c))
		copy_scan(OldTable, put_bulk, bulk, &c);
	sift_tbl_bulk_finish(bulk);
	*xid_cutoff = OldestXmin;
	*multi_cutoff = GetOldestMultiXactId();
}

typedef struct ValidateCtx
{
	Relation	table_rel;
	Relation	index_rel;
	IndexInfo  *info;
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	ValidateIndexState *state;
} ValidateCtx;

/* The row in the slot is inserted into the index at its address, when the index predicate holds */
static void
validate_insert(ValidateCtx *v, ItemPointer tid)
{
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];

	MemoryContextReset(v->econtext->ecxt_per_tuple_memory);
	if (v->info->ii_PredicateState != NULL && !ExecQual(v->info->ii_PredicateState, v->econtext))
		return;
	FormIndexDatum(v->info, v->slot, v->estate, values, isnull);
	index_insert(v->index_rel, values, isnull, tid, v->table_rel,
				 v->info->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO, false, v->info);
	v->state->tups_inserted += 1;
}

/* The address of a row in the index: the entries of a chain are at its root, where the other indexes have them */
static void
validate_root(const SiftTblRel *st, ItemPointer tid)
{
	uint64		key = sift_key_from_tid(tid),
				pred;

	while (sift_tomb_hot_pred(&st->tomb, key, &pred))
		key = pred;
	sift_key_to_tid(key, tid);
}

/* The addresses the index has: the batch already read, then the rest of the sorted stream */
typedef struct IndexAddrs
{
	const TidList *head;
	uint64		pos;
	ValidateIndexState *state;
	Datum		next;
	bool		more;
} IndexAddrs;

static bool
index_addr_next(IndexAddrs *a, int64 *addr)
{
	bool		isnull;

	if (a->pos < a->head->n)
		*addr = (int64) itemptr_encode(&a->head->tid[a->pos++]);
	else if (a->more)
	{
		*addr = DatumGetInt64(a->next);
		a->state->itups += 1;
		a->more = tuplesort_getdatum(a->state->tuplesort, true, false, &a->next, &isnull, NULL);
	}
	else
		return false;
	return true;
}

/* One scan of the table whatever the memory: the addresses of the visible rows (root and own) sorted, and merged with the sorted addresses of the index */
static void
validate_merge(ValidateCtx *v, Snapshot snapshot, IndexAddrs *idx)
{
	SiftTblRel *st = sift_tbl_rel(v->table_rel);
	TupleDesc	desc = CreateTemplateTupleDesc(2);
	AttrNumber	sortcol = 1;
	Oid			sortop = Int8LessOperator,
				coll = InvalidOid;
	bool		nulls_first = false;
	Tuplesortstate *mine;
	TupleTableSlot *put,
			   *rows;
	TableScanDesc scan;
	int64		addr = 0;
	bool		have = index_addr_next(idx, &addr);

	TupleDescInitEntry(desc, 1, "root", INT8OID, -1, 0);
	TupleDescInitEntry(desc, 2, "own", INT8OID, -1, 0);
	sift_tupdesc_finalize(desc);
	mine = tuplesort_begin_heap(desc, 1, &sortcol, &sortop, &coll, &nulls_first, maintenance_work_mem, NULL, TUPLESORT_NONE);
	put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	rows = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	scan = sift_table_beginscan(v->table_rel, snapshot, 0, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, v->slot))
	{
		ItemPointerData root = v->slot->tts_tid;

		CHECK_FOR_INTERRUPTS();
		validate_root(st, &root);
		ExecClearTuple(put);
		put->tts_values[0] = Int64GetDatum((int64) itemptr_encode(&root));
		put->tts_values[1] = Int64GetDatum((int64) itemptr_encode(&v->slot->tts_tid));
		put->tts_isnull[0] = put->tts_isnull[1] = false;
		ExecStoreVirtualTuple(put);
		tuplesort_puttupleslot(mine, put);
		v->state->htups += 1;
	}
	table_endscan(scan);
	tuplesort_performsort(mine);
	while (tuplesort_gettupleslot(mine, true, false, rows, NULL))
	{
		bool		isnull;
		int64		root = DatumGetInt64(slot_getattr(rows, 1, &isnull));
		ItemPointerData own,
					tid;

		CHECK_FOR_INTERRUPTS();
		while (have && addr < root)
			have = index_addr_next(idx, &addr);
		if (have && addr == root)
			continue;
		itemptr_decode(&tid, root);
		itemptr_decode(&own, DatumGetInt64(slot_getattr(rows, 2, &isnull)));
		if (table_tuple_fetch_row_version(v->table_rel, &own, snapshot, v->slot))
			validate_insert(v, &tid);
	}
	ExecDropSingleTupleTableSlot(put);
	ExecDropSingleTupleTableSlot(rows);
	tuplesort_end(mine);
}

/*
 * The rows the first pass did not index are inserted. The TIDs it found, sorted, are held when they fit in maintenance_work_mem (6 bytes a row would be
 * 1.5 GB for 260 million): the table is scanned once and a row is looked for in them. When they do not, the rows of the table are sorted by address as well,
 * and the two sorted streams are merged: one scan of the table whatever the size.
 */
void
sift_tbl_index_validate_scan(Relation table_rel, Relation index_rel, IndexInfo *indexInfo, Snapshot snapshot,
							 ValidateIndexState *state)
{
	EState	   *estate = CreateExecutorState();
	ValidateCtx v = {table_rel, index_rel, indexInfo, estate, GetPerTupleExprContext(estate), table_slot_create(table_rel, NULL), state};
	uint64		cap = Max((uint64) maintenance_work_mem * 1024 / sizeof(ItemPointerData), 1024);
	TidList		have = {.max = cap};
	SiftTblRel *st = sift_tbl_rel(table_rel);
	IndexAddrs	idx = {&have, 0, state};
	bool		isnull;
	TableScanDesc scan;

	v.econtext->ecxt_scantuple = v.slot;
	indexInfo->ii_PredicateState = ExecPrepareQual(indexInfo->ii_Predicate, estate);
	idx.more = tuplesort_getdatum(state->tuplesort, true, false, &idx.next, &isnull, NULL);
	while (idx.more && have.n < cap)
	{
		itemptr_decode(tid_push(&have), DatumGetInt64(idx.next));
		state->itups += 1;
		idx.more = tuplesort_getdatum(state->tuplesort, true, false, &idx.next, &isnull, NULL);
	}
	if (idx.more)
		validate_merge(&v, snapshot, &idx);
	else
	{
		scan = sift_table_beginscan(table_rel, snapshot, 0, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, v.slot))
		{
			ItemPointerData tid = v.slot->tts_tid;

			CHECK_FOR_INTERRUPTS();
			validate_root(st, &tid);
			state->htups += 1;
			if (!tid_has(&tid, &have))
				validate_insert(&v, &tid);
		}
		table_endscan(scan);
	}
	ExecDropSingleTupleTableSlot(v.slot);
	FreeExecutorState(estate);
	indexInfo->ii_PredicateState = NULL;
	if (have.tid)
		pfree(have.tid);
}
