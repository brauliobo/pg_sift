/*
 * col/track_rows.c - the trigger function of an index with track_changes (sift.col_track_old_rows): what a statement trigger (with the transition table of the old rows) or a row trigger hands
 * to the tombstone log of the index it was made for. The install of the triggers and the completeness of the log are in col/track.c.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/htup_details.h"
#include "catalog/pg_am_d.h"
#include "access/skey.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_index.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "catalog/pg_trigger.h"
#include "partitioning/partdesc.h"
#include "utils/partcache.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "access/xlog.h"
#include "storage/lmgr.h"
#include "storage/sinval.h"
#include "utils/timestamp.h"

#include "agg/stats.h"
#include "bm25/maint.h"
#include "col/am.h"
#include "core/tomb.h"

PG_FUNCTION_INFO_V1(sift_col_track_old_rows);

static Oid
trigger_index(Trigger *trig)
{
	return sift_col_track_trigger_index(trig->tgoid);
}

/*
 * An index in service: DROP INDEX CONCURRENTLY marks it dead (indislive) before it waits for the transactions that could still use it, then deletes it holding its lock until it
 * commits. A writer that gets past that mark skips it (it is not read, and a lock on it would wait for the drop, which waits for the writer's lock on the table). An index that is
 * deleted (InvalidOid: the trigger's index is gone) is not in service either.
 */
static bool
index_live(Oid indexoid)
{
	HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
	bool		live = HeapTupleIsValid(tup) && ((Form_pg_index) GETSTRUCT(tup))->indislive;

	if (HeapTupleIsValid(tup))
		ReleaseSysCache(tup);
	return live;
}

/*
 * What the row triggers of track_changes = ctid remember: the index of the trigger now firing (the catalog is asked once, not for each row), and the roots of the chains of the page
 * of the last version they ended. The entries of the index are at the address of the root of a HOT chain, and a version that was made by a HOT update is not: the roots of a page are read once
 * for the versions of it that a statement ends. The index is kept until a relcache invalidation names it, the roots until the transaction ends, because a command number is not the same in the next one.
 */
static struct
{
	Oid			trigger;
	Oid			index;
	uint32		index_hash;		/* of its pg_class row in the RELOID syscache */
	Oid			rel;
	RelFileNumber relnumber;
	BlockNumber blk;
	CommandId	cid;
	bool		live;
	bool		roots_read;
	bool		hooked;
	OffsetNumber roots[MaxHeapTuplesPerPage];
} rowmemo;

static void
rowmemo_reset(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT || event == XACT_EVENT_PREPARE)
		rowmemo.roots_read = false;
}

/* REINDEX CONCURRENTLY swaps what the trigger depends on from the old index to the new one, and invalidates both: the next row asks the catalog again */
static void
rowmemo_inval(Datum arg, int cacheid, uint32 hashvalue)
{
	if (hashvalue == 0 || hashvalue == rowmemo.index_hash)
		rowmemo.trigger = InvalidOid;
}

static OffsetNumber
chain_root(Relation heap, ItemPointer tid)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	CommandId	cid = GetCurrentCommandId(false);
	OffsetNumber root;

	if (!rowmemo.roots_read || rowmemo.rel != RelationGetRelid(heap) || rowmemo.relnumber != heap->rd_locator.relNumber || rowmemo.blk != blk || rowmemo.cid != cid)
	{
		Buffer		buf = ReadBuffer(heap, blk);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		heap_get_root_tuples(BufferGetPage(buf), rowmemo.roots);
		UnlockReleaseBuffer(buf);
		rowmemo.rel = RelationGetRelid(heap);
		rowmemo.relnumber = heap->rd_locator.relNumber;
		rowmemo.blk = blk;
		rowmemo.cid = cid;
		rowmemo.roots_read = true;
	}
	root = rowmemo.roots[ItemPointerGetOffsetNumber(tid) - 1];
	return OffsetNumberIsValid(root) ? root : ItemPointerGetOffsetNumber(tid);
}

/*
 * The row trigger of track_changes = ctid (AFTER UPDATE and AFTER DELETE, for each row): a tombstone for the address of the version the statement ended, held with the transaction's others
 * (sift_col_tomb_hold_tid). A version made by a HOT update has no entry and its predecessor's entry stays right (the indexed columns are the same), so that update writes nothing; any other
 * update writes the entry of its new version at its aminsert, which this tombstone ends the old one for.
 */
static Datum
track_row(TriggerData *td)
{
	Relation	index;
	ItemPointerData tid;

#ifdef SIFT_TESTING
	if (sift_test_no_track)
		return PointerGetDatum(NULL);
#endif
	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event) && HeapTupleHeaderIsHeapOnly(td->tg_newtuple->t_data))
		return PointerGetDatum(NULL);
	tid = td->tg_trigtuple->t_self;
	if (HeapTupleHeaderIsHeapOnly(td->tg_trigtuple->t_data)
#ifdef SIFT_TESTING
		&& !sift_test_no_chain_root
#endif
		)
		ItemPointerSetOffsetNumber(&tid, chain_root(td->tg_relation, &tid));
	if (!rowmemo.hooked)
	{
		RegisterXactCallback(rowmemo_reset, NULL);
		CacheRegisterSyscacheCallback(RELOID, rowmemo_inval, (Datum) 0);
		rowmemo.hooked = true;
	}
	if (rowmemo.trigger != td->tg_trigger->tgoid)
	{
		rowmemo.index = trigger_index(td->tg_trigger);
		rowmemo.index_hash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(rowmemo.index));
		rowmemo.live = index_live(rowmemo.index);
		if (!OidIsValid(rowmemo.index))
			sift_col_track_orphan_fired();
		rowmemo.trigger = td->tg_trigger->tgoid;
	}
	if (!rowmemo.live)
		return PointerGetDatum(NULL);
	index = index_open(rowmemo.index, RowExclusiveLock);
	sift_col_tomb_hold_tid(index, &tid);
	if (sift_conn_journal_on(index))
		sift_conn_ended_row(index, td->tg_relation, td->tg_trigtuple);
	index_close(index, NoLock);
	return PointerGetDatum(NULL);
}

/*
 * May an UPDATE of these columns of rel move a row to another partition (the delete of the old version is then in no replacement of the entry): a column of the partition key is among them
 * (an expression of the key: any column), or a partition is partitioned itself and its own key is not looked at.
 */
static bool
update_may_move_rows(Relation rel, const Bitmapset *updated)
{
	PartitionKey key;
	PartitionDesc desc;

	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		return false;
	key = RelationGetPartitionKey(rel);
	for (int i = 0; i < key->partnatts; i++)
		if (get_partition_col_attnum(key, i) == 0 || bms_is_member(get_partition_col_attnum(key, i) - FirstLowInvalidHeapAttributeNumber, updated))
			return true;
	desc = RelationGetPartitionDesc(rel, true);
	for (int i = 0; i < desc->nparts; i++)
		if (!desc->is_leaf[i])
			return true;
	return false;
}

/*
 * sift.col_track_old_rows(): the trigger of the option; its argument is the oid of the index it was made for (trigger_index says which one it serves now). For a key column it is a statement
 * trigger with the old rows, on the table of the index and on every table above it (a statement on a parent fires the triggers of the parent alone, and its transition table holds the old rows of
 * all its children), for track_changes = ctid a row trigger on the table of the index.
 */
Datum
sift_col_track_old_rows(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	Relation	index,
				heap,
				old_rel;
	AttrNumber	oatt;
	Oid			indexoid,
				heapoid;

	if (CALLED_AS_TRIGGER(fcinfo) && TRIGGER_FIRED_AFTER(td->tg_event) && TRIGGER_FIRED_FOR_ROW(td->tg_event) && !TRIGGER_FIRED_BY_INSERT(td->tg_event))
		return track_row(td);
	if (!CALLED_AS_TRIGGER(fcinfo) || !TRIGGER_FIRED_AFTER(td->tg_event) || TRIGGER_FIRED_FOR_ROW(td->tg_event) || td->tg_oldtable == NULL)
		ereport(ERROR, (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						errmsg("sift.col_track_old_rows is the trigger of an index option: AFTER UPDATE or DELETE, FOR EACH STATEMENT with an old table, or FOR EACH ROW")));
	old_rel = td->tg_relation;
	indexoid = trigger_index(td->tg_trigger);
	if (!OidIsValid(indexoid))
		sift_col_track_orphan_fired();
	if (!index_live(indexoid))
		return PointerGetDatum(NULL);
	/* the old rows of a table above that of the index are in it only if the statement changes it: a partition it pruned is not locked for nothing */
	heapoid = IndexGetRelation(indexoid, false);
	if (heapoid != RelationGetRelid(old_rel) && !sift_col_ends_rows(heapoid))
		return PointerGetDatum(NULL);
	index = index_open(indexoid, RowExclusiveLock);
#ifdef SIFT_TESTING
	if (sift_test_no_track)
	{
		index_close(index, NoLock);
		return PointerGetDatum(NULL);
	}
#endif
	oatt = sift_col_row_key_attno(index, old_rel);
	heap = heapoid == RelationGetRelid(old_rel) ? old_rel : table_open(heapoid, RowExclusiveLock);
	/*
	 * An UPDATE that does not assign the key column (and no BEFORE ROW trigger that could) leaves every row its key: the version it ends has its entry tombstoned by the
	 * replacement (sift_col_supersede, which runs for every row of an UPDATE that writes a new entry), and one a HOT update left is live. Nothing is left to find, unless the
	 * index is partial: a version outside its predicate gets no entry, so no replacement, and the entry of the version before it is the trigger's to tombstone; or the row moved
	 * to another partition, which the replacement of the new partition's index does not see.
	 */
	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event) && td->tg_updatedcols != NULL && !bms_is_member(oatt - FirstLowInvalidHeapAttributeNumber, td->tg_updatedcols) &&
		(heap->trigdesc == NULL || !heap->trigdesc->trig_update_before_row) && RelationGetIndexPredicate(index) == NIL && sift_col_updates_supersede() &&
		!update_may_move_rows(old_rel, td->tg_updatedcols)
#ifdef SIFT_TESTING
		&& !sift_test_no_supersede
#endif
		)
	{
		if (heap != old_rel)
			table_close(heap, NoLock);
		index_close(index, NoLock);
		return PointerGetDatum(NULL);
	}
	sift_col_tomb_dead_keys(index, heap, old_rel, td->tg_oldtable);
	if (heap != old_rel)
		table_close(heap, NoLock);
	index_close(index, NoLock);
	return PointerGetDatum(NULL);
}
