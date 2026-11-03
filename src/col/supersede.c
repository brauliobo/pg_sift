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
#include "access/relation.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "executor/tuptable.h"
#include "utils/lsyscache.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"

#include "col/am.h"
#include "core/compat.h"
#include "core/tomb.h"
#include "core/eval.h"

#ifdef SIFT_TESTING
bool sift_test_no_supersede = false;
bool sift_test_tombs_at_once = false;
bool sift_test_supersede_scan = false;
#endif

/* Transaction-local hints cannot outlive the horizon that prevents reuse of their heap addresses. */
typedef struct LatestHint
{
	Oid index;
	uint32 hash;
	ItemPointerData tid;
} LatestHint;

#define LATEST_HINT_SLOTS 1024
static LatestHint latest[LATEST_HINT_SLOTS];
static bool latest_registered;
StaticAssertDecl(sizeof(latest) <= 16384, "latest candidate hints use at most 16 KiB per backend");

static void
latest_invalidate(Datum arg, Oid relid)
{
	memset(latest, 0, sizeof(latest));
}

static void
latest_xact(XactEvent event, void *arg)
{
	memset(latest, 0, sizeof(latest));
}

static LatestHint *
latest_slot(Relation index, TypeCacheEntry *tc, Oid coll, Datum key)
{
	uint32 hash;

	if (!sift_eval_on(SIFT_EVAL_LATEST_HINT) || !OidIsValid(tc->hash_proc_finfo.fn_oid))
		return NULL;
	if (!latest_registered)
	{
		CacheRegisterRelcacheCallback(latest_invalidate, (Datum)0);
		RegisterXactCallback(latest_xact, NULL);
		latest_registered = true;
	}
	hash = DatumGetUInt32(FunctionCall1Coll(&tc->hash_proc_finfo, coll, key));
	return &latest[(hash ^ RelationGetRelid(index)) % LATEST_HINT_SLOTS];
}

/*
 * Is the version at newtid a speculative insertion (INSERT .. ON CONFLICT before it is confirmed)? It may be killed when the conflict is found, and the row it
 * would replace then stays: no entry is superseded for it. The confirmed insertion of an upsert that finds no conflict replaces nothing either.
 */
static bool
is_speculative(Relation heap, ItemPointer newtid)
{
	Buffer buf = ReadBuffer(heap, ItemPointerGetBlockNumber(newtid));
	Page page;
	ItemId lp;
	bool spec = false;

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	lp = PageGetItemId(page, ItemPointerGetOffsetNumber(newtid));
	if (ItemIdIsNormal(lp))
		spec = HeapTupleHeaderIsSpeculative((HeapTupleHeader)PageGetItem(page, lp));
	UnlockReleaseBuffer(buf);
	return spec;
}

/*
 * The tombstones a statement writes are held back and go to the log by the page (sift_tomb_append_many): written before the log is read by this backend (sift_col_log), before the
 * commit record and before a savepoint starts (col/am/am.c), dropped when the transaction or the savepoint aborts, as the rows of the write buffer are: no other backend can see
 * what they hide before then, since the heap versions they name are of a transaction that has not committed. Each index of the table has its own buffer (an UPDATE gives every
 * one of them its entry for each row, one after the other), so that the tombstones of a statement still go by the page whatever the number of indexes.
 */
#define TOMB_HOLD	2048
#define TOMB_SLOTS	8

typedef struct HeldTombs
{
	Oid			index;
	RelFileNumber relnumber;
	int			n;
	SiftTomb   *ents;
} HeldTombs;

static HeldTombs held[TOMB_SLOTS];

static void
flush_slot(HeldTombs *h)
{
	int			n = h->n;
	Relation	index;

	if (n == 0)
		return;
	h->n = 0;
	if ((index = try_relation_open(h->index, RowExclusiveLock)) != NULL)
	{
		/* a rebuilt index is another file with a log of its own, which holds nothing of these */
		if (index->rd_locator.relNumber == h->relnumber)
			sift_tomb_append_many(index, sift_col_log_appender(index), h->ents, n);
		relation_close(index, NoLock);
	}
}

void
sift_col_tombs_flush(void)
{
	for (int i = 0; i < TOMB_SLOTS; i++)
		flush_slot(&held[i]);
}

void
sift_col_tombs_discard(void)
{
	for (int i = 0; i < TOMB_SLOTS; i++)
		held[i].n = 0;
}

static void
tomb_hold(Relation index, const SiftTomb *ent)
{
	HeldTombs  *h = NULL;

	for (int i = 0; i < TOMB_SLOTS && h == NULL; i++)
		if (held[i].n > 0 && held[i].index == RelationGetRelid(index) && held[i].relnumber == index->rd_locator.relNumber)
			h = &held[i];
	if (h == NULL)
	{
		for (int i = 0; i < TOMB_SLOTS && h == NULL; i++)
			if (held[i].n == 0)
				h = &held[i];
		if (h == NULL)
		{
			sift_col_tombs_flush();
			h = &held[0];
		}
		if (h->ents == NULL)
			h->ents = MemoryContextAlloc(TopMemoryContext, TOMB_HOLD * sizeof(SiftTomb));
		(void) sift_col_log_appender(index);	/* the segment exists (an index that got the option later has none yet) */
		h->index = RelationGetRelid(index);
		h->relnumber = index->rd_locator.relNumber;
	}
	h->ents[h->n++] = *ent;
	if (h->n == TOMB_HOLD
#ifdef SIFT_TESTING
		|| sift_test_tombs_at_once
#endif
		)
		flush_slot(h);
}

/*
 * The entry a new version replaces is the same for every sift index of the table that has the key column: the first one finds it with the probe of the unique index, the others
 * of the same row version take it from here (the new address, the transaction and the command say which row version it is).
 */
static struct
{
	Oid			heap;
	AttrNumber	att;
	ItemPointerData newtid;
	TransactionId xid;
	CommandId	cid;
	bool		found;
	ItemPointerData old;
} probed;
static bool probed_found;
static ItemPointerData probed_old;

/* The key of the version in slot is the key (the point directory lists the fingerprint of a key: another key of the same fingerprint is not the row's), and the entry is not the new version's own: its tombstone */
static bool
tomb_replaced(Relation index, TupleTableSlot *slot, AttrNumber hatt, Form_pg_attribute att, TypeCacheEntry *tc, Datum key, ItemPointer entry,
			  uint64 succ)
{
	bool isnull;
	Datum old = slot_getattr(slot, hatt, &isnull);

	if (isnull || !DatumGetBool(FunctionCall2Coll(&tc->eq_opr_finfo, att->attcollation, old, key)))
		return false;
	tomb_hold(index, &(SiftTomb) {.key = sift_key_from_tid(entry), .xid = GetCurrentTransactionId(), .cid = GetCurrentCommandId(true), .succ = succ, .kind = SIFT_TOMB_UPDATE});
	probed_found = true;
	probed_old = *entry;
	return true;
}

/*
 * The addresses of the key come from the table's unique index on it, which lists every version of a row that has an entry in this one (a version that is not a HOT member
 * has an entry in each index, and the one being updated is not dead, so neither index has dropped it): a probe of a b-tree where the index's own scan opens the
 * segments and reads the buffers. An index scan of the access method is what finds them when the table has no such index now.
 */
static void
supersede_by_unique(Relation index, Relation heap, Oid uoid, Snapshot snap, AttrNumber hatt, Form_pg_attribute att, TypeCacheEntry *tc,
					Datum key, ItemPointer newtid, uint64 succ)
{
	Relation uidx = index_open(uoid, AccessShareLock);
	Oid intype = uidx->rd_opcintype[0];
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

		/* the unique index lets the snapshot see one version of the key: the one it finds is the one replaced, and the entries after it are of older versions that are dead */
		if (!ItemPointerEquals(&entry, newtid) && index_fetch_heap(scan, slot) && tomb_replaced(index, slot, hatt, att, tc, key, &entry, succ))
			break;
	}
	index_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	index_close(uidx, NoLock);
}

static void
supersede_by_scan(Relation index, Relation heap, Snapshot snap, int attno, AttrNumber hatt, Form_pg_attribute att, TypeCacheEntry *tc,
				  Datum key, ItemPointer newtid, uint64 succ)
{
	ScanKeyData skey;
	IndexScanDesc scan;
	ItemPointerData *cand = NULL;
	int ncand = 0, capcand = 0;
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
	fetch = sift_table_index_fetch_begin(heap);
	for (int i = 0; i < ncand; i++)
	{
		ItemPointerData t = cand[i];
		bool again = false, dead = false;

		if (!ItemPointerEquals(&t, newtid) && table_index_fetch_tuple(fetch, &t, snap, slot, &again, &dead))
			tomb_replaced(index, slot, hatt, att, tc, key, &cand[i], succ);
	}
	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
	pfree(cand);
}

void
sift_col_supersede(Relation index, Relation heap, const Datum *values, ItemPointer newtid, bool speculative_stmt)
{
	int attno = sift_col_rel(index)->rowkey_attno;
	AttrNumber hatt = index->rd_index->indkey.values[attno - 1];
	Datum key = values[attno - 1];
	Form_pg_attribute att = TupleDescAttr(RelationGetDescr(heap), hatt - 1);
	TypeCacheEntry *tc = lookup_type_cache(att->atttypid, TYPECACHE_EQ_OPR_FINFO | (sift_eval_on(SIFT_EVAL_LATEST_HINT) ? TYPECACHE_HASH_PROC_FINFO : 0));
	Snapshot snap;
	Oid uoid;
	uint64 succ = sift_key_from_tid(newtid);
	LatestHint *hint;
	uint32 hash = 0;

#ifdef SIFT_TESTING
	if (sift_test_no_supersede)
		return;
#endif
	if (speculative_stmt && is_speculative(heap, newtid))
		return;
	if (probed.heap == RelationGetRelid(heap) && probed.att == hatt && ItemPointerEquals(&probed.newtid, newtid) && probed.xid == GetCurrentTransactionId() &&
		probed.cid == GetCurrentCommandId(true))
	{
		if (probed.found)
			tomb_hold(index, &(SiftTomb) {.key = sift_key_from_tid(&probed.old), .xid = probed.xid, .cid = probed.cid, .succ = succ, .kind = SIFT_TOMB_UPDATE});
		return;
	}
	probed_found = false;
	uoid = sift_col_row_key_unique(heap, hatt);
	snap = RegisterSnapshot(GetLatestSnapshot());
	hint = OidIsValid(uoid) ? latest_slot(index, tc, att->attcollation, key) : NULL;
	if (hint != NULL)
	{
		hash = DatumGetUInt32(FunctionCall1Coll(&tc->hash_proc_finfo, att->attcollation, key));
		if (hint->index == RelationGetRelid(index) && hint->hash == hash && !ItemPointerEquals(&hint->tid, newtid))
		{
			IndexFetchTableData *fetch = sift_table_index_fetch_begin(heap);
			TupleTableSlot *slot = table_slot_create(heap, NULL);
			ItemPointerData tid = hint->tid;
			bool again = false, dead = false, found;

			found = table_index_fetch_tuple(fetch, &tid, snap, slot, &again, &dead) && tomb_replaced(index, slot, hatt, att, tc, key, &hint->tid, succ);
			table_index_fetch_end(fetch);
			ExecDropSingleTupleTableSlot(slot);
			if (found)
				goto done;
		}
	}
#ifdef SIFT_TESTING
	if (sift_test_supersede_scan)
		uoid = InvalidOid;
#endif
	/* the latest committed version (or the transaction's own) is the one being updated: a snapshot of now sees it, the version of this command not */
	if (OidIsValid(uoid))
		supersede_by_unique(index, heap, uoid, snap, hatt, att, tc, key, newtid, succ);
	else
		supersede_by_scan(index, heap, snap, attno, hatt, att, tc, key, newtid, succ);
done:
	if (hint != NULL)
		*hint = (LatestHint){RelationGetRelid(index), hash, *newtid};
	UnregisterSnapshot(snap);
	probed.heap = RelationGetRelid(heap);
	probed.att = hatt;
	probed.newtid = *newtid;
	probed.xid = GetCurrentTransactionId();
	probed.cid = GetCurrentCommandId(true);
	probed.found = probed_found;
	probed.old = probed_old;
}
