/*
 * col/supersede.c - the entries a new row version replaces, for an index with track_updates.
 *
 * An UPDATE that moves a row to a new heap address writes a new entry; the entry of the version it replaced stays in the index until VACUUM. Under the unique index
 * the option requires, the row the key names has one live version, so the entry of the key that the new version does not own is the old one: it gets a tombstone
 * of the transaction and command of the update (the log is voided when the transaction aborts, as the entry of an aborted transaction is). The candidates come from
 * an ordinary scan of the index on the key, snapshot of the statement included: the version being updated is visible to it (the command's own change is not yet),
 * a candidate that is the point directory's fingerprint of another key is dropped by comparing the key in the heap tuple.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"

#include "col/am.h"
#include "core/compat.h"
#include "core/tomb.h"

#ifdef SIFT_TESTING
bool		sift_test_no_supersede = false;
bool		sift_test_supersede_scan = false;
bool		sift_test_track_all_updates = false;
#endif

/*
 * Is the version at newtid a speculative insertion (INSERT .. ON CONFLICT before it is confirmed)? It may be killed when the conflict is found, and the row it
 * would replace then stays: no entry is superseded for it. The confirmed insertion of an upsert that finds no conflict replaces nothing either.
 */
static bool
is_speculative(Relation heap, ItemPointer newtid)
{
	Buffer		buf = ReadBuffer(heap, ItemPointerGetBlockNumber(newtid));
	Page		page;
	ItemId		lp;
	bool		spec = false;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(newtid));
	if (ItemIdIsNormal(lp))
		spec = HeapTupleHeaderIsSpeculative((HeapTupleHeader) PageGetItem(page, lp));
	UnlockReleaseBuffer(buf);
	return spec;
}

/* The key of the version in slot is the key (the point directory lists the fingerprint of a key: another key of the same fingerprint is not the row's), and the entry is not the new version's own: its tombstone */
static void
tomb_replaced(Relation index, SiftTombLog *log, TupleTableSlot *slot, AttrNumber hatt, Form_pg_attribute att, TypeCacheEntry *tc, Datum key, ItemPointer entry, uint64 succ)
{
	bool		isnull;
	Datum		old = slot_getattr(slot, hatt, &isnull);

	if (isnull || !DatumGetBool(FunctionCall2Coll(&tc->eq_opr_finfo, att->attcollation, old, key)))
		return;
	sift_tomb_append(index, log, sift_key_from_tid(entry), GetCurrentTransactionId(), GetCurrentCommandId(true), SIFT_TOMB_UPDATE, 0, succ);
}

/*
 * The addresses of the key come from the table's unique index on it, which lists every version of a row that has an entry in this one (a version that is not a HOT member
 * has an entry in each index, and the one being updated is not dead, so neither index has dropped it): a probe of a b-tree where the index's own scan opens the
 * segments and reads the buffers. An index scan of the access method is what finds them when the table has no such index now.
 */
static void
supersede_by_unique(Relation index, SiftTombLog *log, Relation heap, Oid uoid, Snapshot snap, AttrNumber hatt, Form_pg_attribute att, TypeCacheEntry *tc, Datum key,
					ItemPointer newtid, uint64 succ)
{
	Relation	uidx = index_open(uoid, AccessShareLock);
	Oid			intype = uidx->rd_opcintype[0];
	ScanKeyData skey;
	IndexScanDesc scan;
	TupleTableSlot *slot = table_slot_create(heap, NULL);

	ScanKeyEntryInitialize(&skey, 0, 1, BTEqualStrategyNumber, InvalidOid, uidx->rd_indcollation[0],
						   get_opcode(get_opfamily_member(uidx->rd_opfamily[0], intype, intype, BTEqualStrategyNumber)), key);
	scan = sift_index_beginscan(heap, uidx, snap, 1);
	index_rescan(scan, &skey, 1, NULL, 0);
	for (ItemPointer t; (t = index_getnext_tid(scan, ForwardScanDirection)) != NULL;)
	{
		ItemPointerData entry = *t;

		if (!ItemPointerEquals(&entry, newtid) && index_fetch_heap(scan, slot))
			tomb_replaced(index, log, slot, hatt, att, tc, key, &entry, succ);
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	index_close(uidx, NoLock);
}

static void
supersede_by_scan(Relation index, SiftTombLog *log, Relation heap, Snapshot snap, int attno, AttrNumber hatt, Form_pg_attribute att, TypeCacheEntry *tc, Datum key,
				  ItemPointer newtid, uint64 succ)
{
	ScanKeyData skey;
	IndexScanDesc scan;
	ItemPointerData *cand = NULL;
	int			ncand = 0,
				capcand = 0;
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;

	/* the operator class is polymorphic: the access method reads the strategy, the function of the type's equality is for the form of the key */
	ScanKeyInit(&skey, attno, BTEqualStrategyNumber, tc->eq_opr_finfo.fn_oid, key);
	scan = sift_index_beginscan(heap, index, snap, 1);
	index_rescan(scan, &skey, 1, NULL, 0);
	for (ItemPointer t; (t = index_getnext_tid(scan, ForwardScanDirection)) != NULL;)
	{
		if (ncand == capcand)
		{
			capcand = Max(8, 2 * capcand);
			cand = cand ? repalloc(cand, capcand * sizeof(ItemPointerData)) : palloc(capcand * sizeof(ItemPointerData));
		}
		cand[ncand++] = *t;
	}
	index_endscan(scan);
	if (ncand == 0)
		return;
	slot = table_slot_create(heap, NULL);
	fetch = table_index_fetch_begin(heap);
	for (int i = 0; i < ncand; i++)
	{
		ItemPointerData t = cand[i];
		bool		again = false,
					dead = false;

		if (!ItemPointerEquals(&t, newtid) && table_index_fetch_tuple(fetch, &t, snap, slot, &again, &dead))
			tomb_replaced(index, log, slot, hatt, att, tc, key, &cand[i], succ);
	}
	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
	pfree(cand);
}

void
sift_col_supersede(Relation index, Relation heap, const Datum *values, ItemPointer newtid, bool speculative_stmt)
{
	int			attno = sift_col_rel(index)->rowkey_attno;
	AttrNumber	hatt = index->rd_index->indkey.values[attno - 1];
	Datum		key = values[attno - 1];
	Form_pg_attribute att = TupleDescAttr(RelationGetDescr(heap), hatt - 1);
	TypeCacheEntry *tc = lookup_type_cache(att->atttypid, TYPECACHE_EQ_OPR_FINFO);
	Snapshot	snap;
	SiftTombLog *log;
	Oid			uoid;
	uint64		succ = sift_key_from_tid(newtid);

#ifdef SIFT_TESTING
	if (sift_test_no_supersede)
		return;
#endif
	if (speculative_stmt && is_speculative(heap, newtid))
		return;
	if ((log = sift_col_log(index)) == NULL)
	{
		sift_col_log_create(index);
		log = sift_col_log(index);
	}
	uoid = sift_col_row_key_unique(heap, hatt);
#ifdef SIFT_TESTING
	if (sift_test_supersede_scan)
		uoid = InvalidOid;
#endif
	/* the latest committed version (or the transaction's own) is the one being updated: a snapshot of now sees it, the version of this command not */
	snap = RegisterSnapshot(GetLatestSnapshot());
	if (OidIsValid(uoid))
		supersede_by_unique(index, log, heap, uoid, snap, hatt, att, tc, key, newtid, succ);
	else
		supersede_by_scan(index, log, heap, snap, attno, hatt, att, tc, key, newtid, succ);
	UnregisterSnapshot(snap);
}
