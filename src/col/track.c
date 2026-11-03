/*
 * col/track.c - the statement triggers of an index with track_changes (AFTER UPDATE and AFTER DELETE), and the completeness of its tombstone log.
 *
 * A DELETE, and an UPDATE that changes the key, end a row version without writing a new entry, so the index cannot tell. The triggers (statement, with a transition table of
 * the old rows) hand the old keys to sift.col_track_old_rows, which writes a tombstone for each entry of those keys that no version is live for. The log is complete only with both:
 * before the triggers the deletes were not heard, so the transaction that installs them first writes a tombstone for every row version of the table that is dead now (the
 * baseline), under a lock that holds the writers out. The triggers are the channel: the log keeps the xmin of their catalog rows, and one that is dropped, disabled, enabled again
 * or recreated is another row, which ends the completeness until a new baseline. A read never takes the log for complete on its own word.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

#include "core/eval.h"
#include "col/am.h"
#include "core/tomb.h"

PG_FUNCTION_INFO_V1(sift_col_track_old_rows);
PG_FUNCTION_INFO_V1(sift_col_tracking);
PG_FUNCTION_INFO_V1(sift_col_stamps);

#define HEAR_FUNCTION	"col_track_old_rows"

#ifdef SIFT_TESTING
bool		sift_test_no_track = false;
#endif

/*
 * The two triggers of an index are its AUTO dependents in pg_trigger: [0] AFTER DELETE, [1] AFTER UPDATE (an internal trigger gets its oid appended to its name,
 * so they are found by what depends on the index). Each is checked to be what the option created: on the table, calling the function, a statement trigger, always
 * enabled. witness is its identity: the xmin of its catalog row, which any change to the row (disabling and enabling it again, a rename) makes another.
 */
typedef struct HearTrig
{
	Oid			oid;
	bool		ok;
	uint32		witness;
	int			n;
} HearTrig;

/* By the schema oid, not LookupFuncName: a qualified name asks for USAGE on the schema, which an owner of the table creating the index need not have. */
static Oid
track_function(bool missing_ok)
{
	Oid			nsp = get_namespace_oid("sift", missing_ok);
	Oid			fn = OidIsValid(nsp) ? GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid, CStringGetDatum(HEAR_FUNCTION),
													   PointerGetDatum(buildoidvector(NULL, 0)), ObjectIdGetDatum(nsp)) : InvalidOid;

	SIFT_INVARIANT(OidIsValid(fn) || missing_ok, "sift: function sift.%s() does not exist", HEAR_FUNCTION);
	return fn;
}

static void
read_trigger(Oid heapoid, Oid funcoid, Oid toid, HearTrig t[2])
{
	Relation	trel = table_open(TriggerRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tp;

	ScanKeyInit(&key, Anum_pg_trigger_oid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(toid));
	scan = systable_beginscan(trel, TriggerOidIndexId, true, NULL, 1, &key);
	if (HeapTupleIsValid(tp = systable_getnext(scan)))
	{
		Form_pg_trigger tg = (Form_pg_trigger) GETSTRUCT(tp);
		HearTrig   *h = (tg->tgtype & TRIGGER_TYPE_DELETE) ? &t[0] : (tg->tgtype & TRIGGER_TYPE_UPDATE) ? &t[1] : NULL;

		if (h != NULL)
		{
			h->oid = toid;
			h->n++;
			h->witness = HeapTupleHeaderGetRawXmin(tp->t_data);
			h->ok = tg->tgrelid == heapoid && tg->tgfoid == funcoid && tg->tgenabled == TRIGGER_FIRES_ALWAYS && !tg->tgdeferrable &&
				(tg->tgtype & (TRIGGER_TYPE_ROW | TRIGGER_TYPE_BEFORE | TRIGGER_TYPE_INSTEAD)) == 0 && tg->tgattr.dim1 == 0;
		}
	}
	systable_endscan(scan);
	table_close(trel, AccessShareLock);
}

static void
find_triggers(Oid heapoid, Relation index, HearTrig t[2])
{
	Relation	drel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			funcoid = track_function(true);

	memset(t, 0, 2 * sizeof(HearTrig));
	/* the extension is not updated to the release that has the function yet (the library is: ALTER INDEX of any option asks): no trigger can be its own */
	if (!OidIsValid(funcoid))
		return;
	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(index)));
	scan = systable_beginscan(drel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == TriggerRelationId && d->deptype == DEPENDENCY_AUTO && d->refobjsubid == 0)
			read_trigger(heapoid, funcoid, d->objid, t);
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
}

/* The triggers installed: present[i] at all, ok[i] once and as the option made it, and their identities in witness */
static void
channel_read(Relation heap, Relation index, bool present[2], bool ok[2], uint32 witness[2])
{
	HearTrig	t[2];

	find_triggers(RelationGetRelid(heap), index, t);
	for (int i = 0; i < 2; i++)
	{
		present[i] = t[i].n > 0;
		ok[i] = t[i].ok && t[i].n == 1;
		witness[i] = t[i].witness;
	}
}

static bool subscribed(Oid heapoid);

/* Why a statement trigger of the table cannot hear what happens to its rows, NULL when it can (a statement on the parent fires no trigger of a child; the apply worker of a subscription fires none) */
const char *
sift_col_track_deaf(Relation heap)
{
	if (heap->rd_rel->relispartition || has_superclass(RelationGetRelid(heap)) || find_inheritance_children(RelationGetRelid(heap), NoLock) != NIL)
		return "the table is a partition or has inheritance children, and a statement trigger does not fire for a statement on the parent";
	if (subscribed(RelationGetRelid(heap)))
		return "the table is the target of a logical replication subscription, and the apply worker fires no statement trigger";
	return NULL;
}

/* Are both triggers there as the option made them, and the log's own word on its completeness the one the channel gives (complete, with the witnesses of the triggers) */
static bool
channel_as_asked(Relation heap, Relation index, SiftTombLog *log)
{
	bool		present[2],
				ok[2];
	uint32		witness[2];

	channel_read(heap, index, present, ok, witness);
	return ok[0] && ok[1] && log != NULL && log->chan.flags == SIFT_TOMB_COMPLETE && log->chan.witness[0] == witness[0] && log->chan.witness[1] == witness[1];
}

static void
create_trigger(Relation heap, Relation index, bool update)
{
	CreateTrigStmt *stmt = makeNode(CreateTrigStmt);
	TriggerTransition *tt = makeNode(TriggerTransition);
	Oid			funcoid = track_function(false);
	ObjectAddress trig,
				idx;

	tt->name = "old_rows";
	tt->isNew = false;
	tt->isTable = true;
	stmt->trigname = psprintf(update ? "sift_track_update_%s" : "sift_track_delete_%s", RelationGetRelationName(index));
	stmt->relation = makeRangeVar(get_namespace_name(RelationGetNamespace(heap)), pstrdup(RelationGetRelationName(heap)), -1);
	stmt->funcname = list_make2(makeString("sift"), makeString(HEAR_FUNCTION));
	stmt->args = list_make1(makeString(psprintf("%u", RelationGetRelid(index))));
	stmt->row = false;
	stmt->timing = TRIGGER_TYPE_AFTER;
	stmt->events = update ? TRIGGER_TYPE_UPDATE : TRIGGER_TYPE_DELETE;
	stmt->transitionRels = list_make1(tt);
	trig = CreateTriggerFiringOn(stmt, "", RelationGetRelid(heap), InvalidOid, InvalidOid, InvalidOid, funcoid, InvalidOid, NULL, true, false, TRIGGER_FIRES_ALWAYS);
	ObjectAddressSet(idx, RelationRelationId, RelationGetRelid(index));
	recordDependencyOn(&trig, &idx, DEPENDENCY_AUTO);
}

static void
drop_triggers(Relation heap, Relation index)
{
	HearTrig	t[2];

	find_triggers(RelationGetRelid(heap), index, t);
	for (int i = 0; i < 2; i++)
		if (OidIsValid(t[i].oid))
		{
			ObjectAddress obj;

			ObjectAddressSet(obj, TriggerRelationId, t[i].oid);
			performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
		}
	CommandCounterIncrement();
}

/*
 * The baseline: a tombstone for every address of the table an index entry can name (a root of a chain, a redirect, a dead item) that no version is live for. The
 * caller holds the writers out (the lock of CREATE INDEX and REINDEX, ShareRowExclusiveLock for ALTER INDEX), so what is dead now is dead for good. An address no
 * entry names is harmless: VACUUM drops the tombstone when it frees the address.
 */
static void
baseline(Relation index, Relation heap, SiftTombLog *log)
{
	SnapshotData dirty;
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	TransactionId xid = GetCurrentTransactionId();
	CommandId	cid = GetCurrentCommandId(true);

	InitDirtySnapshot(dirty);
	for (BlockNumber blk = 0; blk < nblocks; blk++)
	{
		Buffer		buf = ReadBuffer(heap, blk);
		Page		page;
		OffsetNumber roots[MaxHeapTuplesPerPage];
		bool		gone[MaxHeapTuplesPerPage];
		int			n = 0;

		CHECK_FOR_INTERRUPTS();
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		for (OffsetNumber off = FirstOffsetNumber, max = PageGetMaxOffsetNumber(page); off <= max; off++)
		{
			ItemId		lp = PageGetItemId(page, off);

			if (!ItemIdIsUsed(lp) || (ItemIdIsNormal(lp) && HeapTupleHeaderIsHeapOnly((HeapTupleHeader) PageGetItem(page, lp))))
				continue;
			roots[n] = off;
			gone[n++] = ItemIdIsDead(lp);
		}
		UnlockReleaseBuffer(buf);
		for (int i = 0; i < n; i++)
		{
			ItemPointerData tid;
			bool		all_dead;

			ItemPointerSet(&tid, blk, roots[i]);
			if (gone[i] || !table_index_fetch_tuple_check(heap, &tid, &dirty, &all_dead))
				sift_tomb_append(index, log, sift_key_from_tid(&tid), xid, cid, SIFT_TOMB_DELETE, 0, 0);
		}
	}
}

/*
 * Is the table the target of a logical replication subscription? The apply worker changes rows without firing statement triggers, so the deletes it applies would
 * not be heard, and nothing in the log would say so.
 */
static bool
subscribed(Oid heapoid)
{
	Relation	rel = table_open(SubscriptionRelRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	bool		found;

	ScanKeyInit(&key, Anum_pg_subscription_rel_srrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(heapoid));
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 1, &key);
	found = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return found;
}

/* track_changes names a column on this index: the table must be one a statement trigger hears (the default off, with a NOTICE, is decided before the build, in sift_col_row_key_prepare) */
void
sift_col_track_check(Relation heap, Relation index)
{
	const char *why;

	if (!sift_col_row_key_on(index) || (why = sift_col_track_deaf(heap)) == NULL)
		return;
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("track_changes: table \"%s\" cannot be tracked", RelationGetRelationName(heap)),
					errdetail("%s.", why),
					errhint("WITH (track_changes = 'off') builds the index without it: reads of the index check the heap.")));
}

/*
 * Installs the two triggers and makes the log complete: unless all is in place already (a new baseline whenever a trigger is new or another row). A NOTICE says it.
 */
void
sift_col_track_install(Relation heap, Relation index, bool concurrent)
{
	SiftTombLog *log = sift_col_log(index);
	SiftTombChan chan = {.flags = SIFT_TOMB_COMPLETE};
	bool		present[2],
				ok[2];

	if (log == NULL)
	{
		sift_col_log_create(index);
		log = sift_col_log(index);
	}
	if (channel_as_asked(heap, index, log))
		return;
	drop_triggers(heap, index);
	create_trigger(heap, index, false);
	create_trigger(heap, index, true);
	CommandCounterIncrement();
	ereport(NOTICE, (errmsg("sift_col index \"%s\" installed two statement triggers on table \"%s\" and tracks its changes", RelationGetRelationName(index), RelationGetRelationName(heap)),
					 errdetail("track_changes: AFTER UPDATE and AFTER DELETE, FOR EACH STATEMENT, with a transition table of the old rows; they tombstone the entries of the old key of an UPDATE that changes the key and of every DELETE, after a pass over the heap for the versions already dead. They are dropped with the index."),
					 errhint("WITH (track_changes = 'off') builds the index without them.")));
	/* writers run beside a concurrent build: what they ended before the triggers is not known, so the log is not complete */
	if (concurrent)
	{
		ereport(NOTICE, (errmsg("sift_col index \"%s\": track_changes tracks the changes from now on, but the log is not complete after a concurrent build", RelationGetRelationName(index)),
						 errhint("To make it complete, run ALTER INDEX %s SET (track_changes = 'off') and then ALTER INDEX %s SET (track_changes = '%s'), or REINDEX.",
								 RelationGetRelationName(index), RelationGetRelationName(index), sift_col_options_row_key(index))));
		return;
	}
	baseline(index, heap, log);
	channel_read(heap, index, present, ok, chan.witness);
	SIFT_INVARIANT(ok[0] && ok[1], "the triggers of index %u were not created", RelationGetRelid(index));
	chan.base_xid = GetCurrentTransactionId();
	sift_tomb_set_chan(index, log, &chan);
}

/* The option is off: no trigger and no completeness */
void
sift_col_track_remove(Relation heap, Relation index)
{
	SiftTombLog *log = sift_col_log(index);

	drop_triggers(heap, index);
	if (log != NULL && log->chan.flags != 0)
		sift_tomb_set_chan(index, log, &(SiftTombChan) {0});
}

/* After ALTER INDEX: the triggers follow the option */
void
sift_col_track_reconcile(Relation index)
{
	bool		present[2],
				ok[2];
	uint32		witness[2];
	Relation	heap;
	bool		done;

	/* an ALTER INDEX of any other option must not hold the writers of the table out: lock it only to install or to drop */
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	if (sift_col_row_key_on(index))
	{
		done = channel_as_asked(heap, index, sift_col_log(index));
		table_close(heap, AccessShareLock);
		if (done)
			return;
		heap = table_open(index->rd_index->indrelid, ShareRowExclusiveLock);
		sift_col_track_check(heap, index);
		sift_col_track_install(heap, index, false);
		table_close(heap, NoLock);
		return;
	}
	channel_read(heap, index, present, ok, witness);
	done = !present[0] && !present[1];
	table_close(heap, AccessShareLock);
	if (done)
		return;
	heap = table_open(index->rd_index->indrelid, AccessExclusiveLock);
	sift_col_track_remove(heap, index);
	table_close(heap, NoLock);
}

/*
 * May a read take the log for the whole truth about which entries are live: the log was made complete, the triggers are the ones that did it, and the table still has its unique index on the key. With a snapshot, also that it is an MVCC one that sees the transaction that made the log complete.
 */
bool
sift_col_track_ok(Relation index, Relation heap, Snapshot snap)
{
	bool		present[2],
				ok[2];
	uint32		witness[2];
	SiftTombLog *log;
	SiftColRel *r;

	if (!sift_col_row_key_on(index))
		return false;
	r = sift_col_rel(index);
	if (!OidIsValid(sift_col_row_key_unique(heap, index->rd_index->indkey.values[r->rowkey_attno - 1])) || subscribed(RelationGetRelid(heap)))
		return false;
	if ((log = sift_col_log(index)) == NULL || log->chan.flags != SIFT_TOMB_COMPLETE)
		return false;
	channel_read(heap, index, present, ok, witness);
	if (!ok[0] || !ok[1] || witness[0] != log->chan.witness[0] || witness[1] != log->chan.witness[1])
		return false;
	if (snap == NULL)
		return true;
	return snap->snapshot_type == SNAPSHOT_MVCC &&
		(TransactionIdIsCurrentTransactionId(log->chan.base_xid) || (!XidInMVCCSnapshot(log->chan.base_xid, snap) && sift_xid_state(log->chan.base_xid) == SIFT_XID_COMMITTED));
}

/* sift.col_track_old_rows(): the statement trigger of the option; its argument is the oid of the index */
Datum
sift_col_track_old_rows(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	Relation	index,
				heap;
	AttrNumber	hatt;

	if (!CALLED_AS_TRIGGER(fcinfo) || !TRIGGER_FIRED_AFTER(td->tg_event) || TRIGGER_FIRED_FOR_ROW(td->tg_event) || td->tg_oldtable == NULL)
		ereport(ERROR, (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						errmsg("sift.col_track_old_rows is the statement trigger of an index option: AFTER, FOR EACH STATEMENT, with an old table")));
	heap = td->tg_relation;
	index = index_open(atooid(td->tg_trigger->tgargs[0]), RowExclusiveLock);
#ifdef SIFT_TESTING
	if (sift_test_no_track)
	{
		index_close(index, NoLock);
		return PointerGetDatum(NULL);
	}
#endif
	hatt = index->rd_index->indkey.values[sift_col_rel(index)->rowkey_attno - 1];
	/*
	 * An UPDATE that does not assign the key column (and no BEFORE ROW trigger that could) leaves every row its key: the version it ends has its entry tombstoned by the
	 * replacement (sift_col_supersede, which runs for every row of an UPDATE that writes a new entry), and one a HOT update left is live. Nothing is left to find, unless the
	 * index is partial: a version outside its predicate gets no entry, so no replacement, and the entry of the version before it is the trigger's to tombstone.
	 */
	if (TRIGGER_FIRED_BY_UPDATE(td->tg_event) && td->tg_updatedcols != NULL && !bms_is_member(hatt - FirstLowInvalidHeapAttributeNumber, td->tg_updatedcols) &&
		(heap->trigdesc == NULL || !heap->trigdesc->trig_update_before_row) && RelationGetIndexPredicate(index) == NIL && sift_col_updates_supersede() &&
		sift_eval_on(SIFT_EVAL_SKIP_TRIGGER_LOOKUP)
#ifdef SIFT_TESTING
		&& !sift_test_no_supersede
#endif
		)
	{
		index_close(index, NoLock);
		return PointerGetDatum(NULL);
	}
	sift_col_tomb_dead_keys(index, heap, td->tg_oldtable);
	index_close(index, NoLock);
	return PointerGetDatum(NULL);
}

/* sift.col_tracking(index): what the log can be taken for, and which part is missing */
Datum
sift_col_tracking(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	Relation	heap;
	TupleDesc	desc;
	Datum		v[4];
	bool		nulls[4] = {false};
	SiftTombLog *log;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("function returning record called in context that cannot accept type record"),
						errhint("Call it in the FROM clause: SELECT * FROM sift.col_tracking(index).")));
	if (!sift_col_is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	log = sift_col_row_key_on(index) ? sift_col_log(index) : NULL;
	v[0] = BoolGetDatum(sift_col_row_key_on(index));
	v[1] = BoolGetDatum(log != NULL && channel_as_asked(heap, index, log));
	v[2] = BoolGetDatum(log != NULL && log->chan.flags == SIFT_TOMB_COMPLETE);
	v[3] = BoolGetDatum(sift_col_track_ok(index, heap, NULL));
	table_close(heap, NoLock);
	index_close(index, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

/* The segments of an index and the xid before which every inserter of their entries committed (NULL: the segment does not say, and reads check the heap for its rows) */
Datum
sift_col_stamps(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

	if (!sift_col_is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	InitMaterializedSRF(fcinfo, 0);
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];
		Datum		v[3];
		bool		nulls[3] = {false};
		TransactionId x = InvalidTransactionId;

		if (e->kind != SIFT_SEG_IMMUTABLE)
			continue;
		v[0] = Int64GetDatum(e->gen);
		v[1] = Int64GetDatum(e->ndocs);
		nulls[2] = !sift_seg_xstamp(e, &x);
		v[2] = TransactionIdGetDatum(x);
		tuplestore_putvalues(((ReturnSetInfo *) fcinfo->resultinfo)->setResult, ((ReturnSetInfo *) fcinfo->resultinfo)->setDesc, v, nulls);
	}
	index_close(index, NoLock);
	return (Datum) 0;
}
