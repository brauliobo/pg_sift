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
#include "catalog/pg_trigger.h"
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
#include "utils/timestamp.h"

#include "agg/stats.h"
#include "bm25/maint.h"
#include "col/am.h"
#include "core/tomb.h"

PG_FUNCTION_INFO_V1(sift_col_track_old_rows);
PG_FUNCTION_INFO_V1(sift_col_tracking);
PG_FUNCTION_INFO_V1(sift_col_stamps);

#define HEAR_FUNCTION	"col_track_old_rows"

#ifdef SIFT_TESTING
bool		sift_test_no_track = false;
bool		sift_test_no_chain_root = false;
#endif

/*
 * The two triggers of an index are the rows of pg_trigger that it depends on (NORMAL): [0] AFTER DELETE, [1] AFTER UPDATE (an internal trigger gets its oid appended to its name,
 * so they are found by what the index depends on). The index depends on them, and not they on it, because DROP INDEX CONCURRENTLY refuses to start once a transaction id is
 * assigned and deletes what depends on the index first: an index nothing depends on drops, and sift_col_track_hook removes the triggers in the transaction that deletes the
 * index. REINDEX CONCURRENTLY swaps the dependencies of the old and the new index, so the triggers follow the index that stays. Each is checked to be what the option
 * created: on the table, calling the function, a statement trigger, always enabled. witness is its identity: the xmin of its catalog row, which any change to the row
 * (disabling and enabling it again, a rename) makes another.
 */
typedef struct HearTrig
{
	Oid			oid;
	NameData	name;
	bool		ok;
	bool		origin;			/* what ok says, but enabled for the origin only (ENABLE TRIGGER) */
	uint32		witness;
	int			n;
} HearTrig;

/* By the schema oid, not LookupFuncName: a qualified name asks for USAGE on the schema, which an owner of the table creating the index need not have. */
Oid
sift_col_track_lookup(const char *name, bool missing_ok)
{
	Oid			nsp = get_namespace_oid("sift", missing_ok);
	Oid			fn = OidIsValid(nsp) ? GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid, CStringGetDatum(name),
													   PointerGetDatum(buildoidvector(NULL, 0)), ObjectIdGetDatum(nsp)) : InvalidOid;

	SIFT_INVARIANT(OidIsValid(fn) || missing_ok, "sift: function sift.%s() does not exist", name);
	return fn;
}

Oid
sift_col_track_function(bool missing_ok)
{
	return sift_col_track_lookup(HEAR_FUNCTION, missing_ok);
}

static void
read_trigger(Oid heapoid, Oid funcoid, Oid toid, HearTrig t[2], bool row)
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
			bool		shape = tg->tgrelid == heapoid && tg->tgfoid == funcoid && !tg->tgdeferrable &&
				(tg->tgtype & (TRIGGER_TYPE_BEFORE | TRIGGER_TYPE_INSTEAD)) == 0 && ((tg->tgtype & TRIGGER_TYPE_ROW) != 0) == row && tg->tgattr.dim1 == 0;

			h->oid = toid;
			h->name = tg->tgname;
			h->n++;
			h->witness = HeapTupleHeaderGetRawXmin(tp->t_data);
			h->ok = shape && tg->tgenabled == TRIGGER_FIRES_ALWAYS;
			h->origin = shape && tg->tgenabled == TRIGGER_FIRES_ON_ORIGIN;
		}
	}
	systable_endscan(scan);
	table_close(trel, AccessShareLock);
}

static void
find_triggers(Oid heapoid, Relation index, HearTrig t[2], bool row)
{
	Relation	drel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			funcoid = sift_col_track_function(true);

	memset(t, 0, 2 * sizeof(HearTrig));
	/* the extension is not updated to the release that has the function yet (the library is: ALTER INDEX of any option asks): no trigger can be its own */
	if (!OidIsValid(funcoid))
		return;
	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(index)));
	scan = systable_beginscan(drel, DependDependerIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->refclassid == TriggerRelationId && d->deptype == DEPENDENCY_NORMAL && d->refobjsubid == 0)
			read_trigger(heapoid, funcoid, d->refobjid, t, row);
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
}

/* The triggers installed: present[i] at all, ok[i] once and as the option made it, and their identities in witness */
static void
channel_read(Relation heap, Relation index, bool present[2], bool ok[2], uint32 witness[2])
{
	HearTrig	t[2];

	find_triggers(RelationGetRelid(heap), index, t, sift_col_track_ctid(index));
	for (int i = 0; i < 2; i++)
	{
		present[i] = t[i].n > 0;
		ok[i] = t[i].ok && t[i].n == 1;
		witness[i] = t[i].witness;
	}
}

static bool subscribed(Oid heapoid);
static bool trackable(Relation heap, Relation index);

/*
 * Why a statement trigger of the table cannot hear what happens to its rows, NULL when it can (a statement on the parent fires no trigger of a child; the apply worker of a
 * subscription fires none; pg_upgrade restores the indexes of an older release into an extension that is not updated yet, which has no function for the trigger).
 */
const char *
sift_col_track_deaf(Relation heap)
{
	if (!OidIsValid(sift_col_track_function(true)))
		return "the extension is not updated to the release that has sift.col_track_old_rows(): run ALTER EXTENSION pg_sift UPDATE";
	if (heap->rd_rel->relispartition || has_superclass(RelationGetRelid(heap)) || find_inheritance_children(RelationGetRelid(heap), NoLock) != NIL)
		return "the table is a partition or has inheritance children, and a statement trigger does not fire for a statement on the parent";
	if (subscribed(RelationGetRelid(heap)))
		return "the table is the target of a logical replication subscription, and the apply worker fires no statement trigger";
	return NULL;
}

/* Are both triggers there as the option made them, and the log's own word on its completeness the one the channel gives (complete, with the witnesses of the triggers): of this release or an earlier one */
static bool
channel_vouched(Relation heap, Relation index, SiftTombLog *log)
{
	bool		present[2],
				ok[2];
	uint32		witness[2];

	channel_read(heap, index, present, ok, witness);
	return ok[0] && ok[1] && log != NULL && (log->chan.flags & SIFT_TOMB_COMPLETE) && log->chan.witness[0] == witness[0] && log->chan.witness[1] == witness[1];
}

/* ... and the log was made complete (or re-baselined) by a release that has the fix: an index of an earlier one is installed again by ALTER INDEX, and re-baselined by sift_col_track_rebaseline */
static bool
channel_as_asked(Relation heap, Relation index, SiftTombLog *log)
{
	return channel_vouched(heap, index, log) && log->chan.flags == SIFT_TOMB_CURRENT;
}

static void
create_trigger(Relation heap, Relation index, bool update)
{
	CreateTrigStmt *stmt = makeNode(CreateTrigStmt);
	TriggerTransition *tt = makeNode(TriggerTransition);
	Oid			funcoid = sift_col_track_function(false);
	bool		row = sift_col_track_ctid(index);
	ObjectAddress trig,
				idx;

	tt->name = "old_rows";
	tt->isNew = false;
	tt->isTable = true;
	stmt->trigname = psprintf(update ? "sift_track_update_%s" : "sift_track_delete_%s", RelationGetRelationName(index));
	stmt->relation = makeRangeVar(get_namespace_name(RelationGetNamespace(heap)), pstrdup(RelationGetRelationName(heap)), -1);
	stmt->funcname = list_make2(makeString("sift"), makeString(HEAR_FUNCTION));
	stmt->args = list_make1(makeString(psprintf("%u", RelationGetRelid(index))));
	stmt->row = row;
	stmt->timing = TRIGGER_TYPE_AFTER;
	stmt->events = update ? TRIGGER_TYPE_UPDATE : TRIGGER_TYPE_DELETE;
	stmt->transitionRels = row ? NIL : list_make1(tt);
	trig = CreateTriggerFiringOn(stmt, "", RelationGetRelid(heap), InvalidOid, InvalidOid, InvalidOid, funcoid, InvalidOid, NULL, true, false, TRIGGER_FIRES_ALWAYS);
	ObjectAddressSet(idx, RelationRelationId, RelationGetRelid(index));
	recordDependencyOn(&idx, &trig, DEPENDENCY_NORMAL);
}

static void
drop_triggers(Relation heap, Relation index)
{
	HearTrig	t[2];

	find_triggers(RelationGetRelid(heap), index, t, false);
	for (int i = 0; i < 2; i++)
		if (OidIsValid(t[i].oid))
		{
			deleteDependencyRecordsForSpecific(RelationRelationId, RelationGetRelid(index), DEPENDENCY_NORMAL, TriggerRelationId, t[i].oid);
			CommandCounterIncrement();
			sift_col_track_delete_trigger(t[i].oid);
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

	if (sift_col_track_ctid(index) && heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("track_changes: table \"%s\" is not a heap table", RelationGetRelationName(heap)),
						errdetail("track_changes = 'ctid' tombstones the heap address of a row version."),
						errhint("WITH (track_changes = 'off') builds the index without it: reads of the index check the heap.")));
	if (!sift_col_track_on(index) || (why = sift_col_track_deaf(heap)) == NULL)
		return;
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("track_changes: table \"%s\" cannot be tracked", RelationGetRelationName(heap)),
					errdetail("%s.", why),
					errhint("WITH (track_changes = 'off') builds the index without it: reads of the index check the heap.")));
}

/*
 * The index of the copy of pg_repack (repack.index_<oid>) takes the place of the index of the table by the swap, and the log it ends with is complete for the table: the copy is a
 * full rewrite of the rows visible to a snapshot, and what the writers do meanwhile pg_repack applies to the copy by statements, which its own triggers hear. The triggers that
 * keep it complete from the swap on are those of the table, which pg_repack leaves in place and the swap does not touch: their identities are the log's witnesses, not those of the
 * triggers of the copy, which go with it.
 */
static void
repack_witness(Relation heap, Relation index, uint32 witness[2])
{
	Oid			oheap = sift_col_repack_origin(heap),
				oindex = sift_col_repack_origin(index);
	Relation	orig;
	HearTrig	t[2];

	if (!OidIsValid(oheap) || !OidIsValid(oindex) || (orig = try_relation_open(oindex, AccessShareLock)) == NULL)
		return;
	if (orig->rd_rel->relkind == RELKIND_INDEX && orig->rd_rel->relam == index->rd_rel->relam && orig->rd_index->indrelid == oheap)
	{
		find_triggers(oheap, orig, t, sift_col_track_ctid(orig));
		if (t[0].ok && t[0].n == 1 && t[1].ok && t[1].n == 1)
		{
			witness[0] = t[0].witness;
			witness[1] = t[1].witness;
		}
	}
	relation_close(orig, AccessShareLock);
}

/*
 * The index that a concurrent build replaces by this copy, on the same table, when it has its two triggers: REINDEX CONCURRENTLY (<name>_ccnew, <name>_ccnew1, ..., not valid yet) and
 * pg_repack --only-indexes (index_<oid>, in the schema of the table). They are the triggers of the index after the swap: PostgreSQL moves what depends on the old index to the new one,
 * pg_repack swaps the files and leaves the oids. InvalidOid for any other index, and for an old index that has no triggers of its own, or another key.
 */
Oid
sift_col_track_swap_origin(Relation heap, Relation index)
{
	const char *name = RelationGetRelationName(index);
	const char *p = strstr(name, "_ccnew");
	Oid			origin;
	Relation	orig;
	HearTrig	t[2];

	if (p != NULL && !index->rd_index->indisvalid)
	{
		for (const char *d = p + strlen("_ccnew"); *d != '\0'; d++)
			if (*d < '0' || *d > '9')
				return InvalidOid;
		origin = get_relname_relid(pnstrdup(name, p - name), RelationGetNamespace(index));
	}
	else if (strncmp(name, "index_", 6) == 0)
	{
		char	   *end;
		unsigned long oid = strtoul(name + 6, &end, 10);

		origin = *end == '\0' && oid != 0 ? (Oid) oid : InvalidOid;
	}
	else
		return InvalidOid;
	if (!OidIsValid(origin) || origin == RelationGetRelid(index) || (orig = try_relation_open(origin, AccessShareLock)) == NULL)
		return InvalidOid;
	origin = InvalidOid;
	if (orig->rd_rel->relkind == RELKIND_INDEX && orig->rd_rel->relam == index->rd_rel->relam && orig->rd_index->indrelid == RelationGetRelid(heap) &&
		orig->rd_index->indnkeyatts == index->rd_index->indnkeyatts &&
		memcmp(orig->rd_index->indkey.values, index->rd_index->indkey.values, orig->rd_index->indnkeyatts * sizeof(int16)) == 0)
	{
		find_triggers(RelationGetRelid(heap), orig, t, sift_col_track_ctid(orig));
		if (t[0].n == 1 && t[1].n == 1)
			origin = RelationGetRelid(orig);
	}
	relation_close(orig, AccessShareLock);
	return origin;
}

/*
 * Installs the two triggers and makes the log complete: unless all is in place already (a new baseline whenever a trigger is new or another row). A NOTICE says it.
 */
void
sift_col_track_install(Relation heap, Relation index, bool concurrent)
{
	SiftTombLog *log = sift_col_log(index);
	SiftTombChan chan = {.flags = SIFT_TOMB_CURRENT};
	bool		present[2],
				ok[2];

	if (log == NULL)
	{
		sift_col_log_create(index);
		log = sift_col_log(index);
	}
	if (channel_as_asked(heap, index, log))
		return;
	/* the copy of a concurrent swap has none of its own: dropping the old index would drop them with it, and DROP TRIGGER takes the lock that no writer gets past */
	if (!concurrent || !OidIsValid(sift_col_track_swap_origin(heap, index)))
	{
		drop_triggers(heap, index);
		create_trigger(heap, index, false);
		create_trigger(heap, index, true);
		CommandCounterIncrement();
	}
	/* writers run beside a concurrent build: what they ended before the triggers is not known, so the log is not complete */
	if (concurrent)
	{
		ereport(NOTICE, (errmsg("sift_col index \"%s\": track_changes log is not complete after a concurrent build", sift_col_final_name(index)),
						 errhint("The next VACUUM completes the log; REINDEX INDEX %s without CONCURRENTLY does it now.", sift_col_final_name(index))));
		return;
	}
	baseline(index, heap, log);
	channel_read(heap, index, present, ok, chan.witness);
	SIFT_INVARIANT(ok[0] && ok[1], "the triggers of index %u were not created", RelationGetRelid(index));
	repack_witness(heap, index, chan.witness);
	chan.base_xid = GetCurrentTransactionId();
	sift_tomb_set_chan(index, log, &chan);
}

/*
 * Is the log due for the re-baseline: the triggers do what the option asks and the log is not current for them. That is a log written before the fix (flags COMPLETE alone: 4.0.0 to 4.1.1),
 * one made while the triggers were not there (a concurrent build, a table that was a partition) or not these (pg_upgrade and a restore make the triggers again, as CREATE INDEX does, and the log
 * names the identities of the old ones), and one whose re-baseline began and aborted (the flags are set in place, the transaction is not: the log then names a transaction that never
 * committed). The identities the triggers have now are returned in witness. Triggers that are disabled, or enabled for the origin only, do not do what the option asks: that log waits for
 * ALTER INDEX or REINDEX.
 */
static bool
rebaseline_due(Relation heap, Relation index, const SiftTombLog *log, uint32 witness[2])
{
	bool		present[2],
				ok[2];

	channel_read(heap, index, present, ok, witness);
	if (!ok[0] || !ok[1])
		return false;
	return log->chan.flags != SIFT_TOMB_CURRENT || witness[0] != log->chan.witness[0] || witness[1] != log->chan.witness[1] ||
		(TransactionIdIsNormal(log->chan.base_xid) && sift_xid_state(log->chan.base_xid) == SIFT_XID_ABORTED);
}

/*
 * The repair of a log that cannot be taken for complete (rebaseline_due): the statements of a library before the fix that moved the command id left row versions without a tombstone (every
 * release since 4.0.0, docs/releases/v4.1.1.md), and statements that ran while the triggers were not there or not these (a concurrent build, a restore, pg_upgrade) may have too. The baseline of
 * the install does what is needed, with the triggers as they are: a tombstone for every address of the table that no version is live for, then the log is complete from this transaction on and
 * names the triggers as they are. The caller holds ShareUpdateExclusiveLock on the table, which keeps a VACUUM out (it frees an address that a new row then takes, and the tombstone of the
 * baseline would hide that row), keeps the triggers as they are (every change to one takes a stronger lock), and lets the writers go on: a version a writer ends meanwhile gets its tombstone
 * from the trigger, and the readers whose snapshot does not see this transaction check the heap (sift_col_track_ok). 71 ms (a vacuumed table) to 119 ms (30,000 rows each updated, deleted and
 * inserted since the VACUUM) for 600,000 rows, 3 kB to 14 MB of WAL. Not on a standby, which has the result by replication, and reads the heap until then.
 * Returns whether it ran: not for a log that is current, an index that does not track, or triggers that are not what the option made (the install or a REINDEX makes that one complete).
 */
bool
sift_col_track_rebaseline(Relation index, Relation heap)
{
	SiftTombLog *log;
	SiftTombChan chan;

	Assert(CheckRelationLockedByMe(heap, ShareUpdateExclusiveLock, true));
	if (RecoveryInProgress() || !trackable(heap, index) || (log = sift_col_log(index)) == NULL || !rebaseline_due(heap, index, log, chan.witness))
		return false;
	baseline(index, heap, log);
	chan.flags = SIFT_TOMB_CURRENT;
	chan.base_xid = GetCurrentTransactionId();
	sift_tomb_set_chan(index, log, &chan);
	return true;
}

/*
 * Triggers that are the option's but for being enabled for the origin only: what ENABLE TRIGGER makes of them after DISABLE TRIGGER ALL (pg_restore --disable-triggers does both). They fire
 * for a session with the usual session_replication_role and not for one with replica (a data loader), which writes unheard, so the log is not taken for complete; a trigger that is disabled is
 * the user's decision and stays so.
 */
bool
sift_col_track_rearmable(Relation index, Relation heap)
{
	HearTrig	t[2];

	find_triggers(RelationGetRelid(heap), index, t, sift_col_track_ctid(index));
	return t[0].n == 1 && t[1].n == 1 && (t[0].ok || t[0].origin) && (t[1].ok || t[1].origin) && (t[0].origin || t[1].origin);
}

/*
 * Enables them for every session again, as ALTER INDEX does by making them: the catalog change of ENABLE ALWAYS TRIGGER, which takes the ShareRowExclusiveLock of the table that the caller
 * holds (and keeps the writers out of it until it commits: a catalog row or two). The log is not complete for them yet: the re-baseline follows, under the lock that lets writers in.
 */
bool
sift_col_track_rearm(Relation index, Relation heap)
{
	HearTrig	t[2];

	Assert(CheckRelationLockedByMe(heap, ShareRowExclusiveLock, true));
	if (RecoveryInProgress() || !trackable(heap, index) || !sift_col_track_rearmable(index, heap))
		return false;
	find_triggers(RelationGetRelid(heap), index, t, sift_col_track_ctid(index));
	for (int i = 0; i < 2; i++)
		if (t[i].origin)
			EnableDisableTrigger(heap, NameStr(t[i].name), InvalidOid, TRIGGER_FIRES_ALWAYS, false, false, ShareRowExclusiveLock);
	CommandCounterIncrement();
	return true;
}

/*
 * A read that takes the heap for a log that may be due for the re-baseline asks the worker for it, once per naptime for an index in this backend and under the backoff of the index
 * (the table may be one nobody writes or vacuums). The maintenance worker and VACUUM re-baseline it as well, whichever comes first; sift.col_auto_merge = off leaves it to VACUUM.
 */
void
sift_col_track_ask(Relation index, Relation heap)
{
	static Oid	asked = InvalidOid;
	static TimestampTz asked_at = 0;
	SiftTombLog *log;
	TimestampTz now;
	uint32		witness[2];

	if (!sift_col_auto_merge || !sift_async_maintenance || !IsUnderPostmaster || IsParallelWorker() || RecoveryInProgress() ||
		index->rd_rel->relpersistence == RELPERSISTENCE_TEMP || !sift_col_track_on(index))
		return;
	now = GetCurrentTimestamp();
	if (asked == RelationGetRelid(index) && now - asked_at < (int64) sift_maintenance_naptime * 1000)
		return;
	asked = RelationGetRelid(index);
	asked_at = now;
	if ((log = sift_col_log(index)) != NULL && (rebaseline_due(heap, index, log, witness) || sift_col_track_rearmable(index, heap)) &&
		sift_maint_request_worker(index, "sift_col_track_worker_main"))
		sift_stat_add(SIFT_STAT_MAINT_ASKS, 1);
}

/*
 * The table became a partition (ATTACH PARTITION) or an inheritance child (INHERIT): a statement on its parent fires none of its statement triggers, so what the parent's statements delete or
 * re-key is not in the log of its indexes, and stays missing once the table is a plain table again (DETACH, NO INHERIT): the log is not complete until a new baseline (REINDEX). Called by the
 * event trigger of ALTER TABLE, which runs in every session whether the library was loaded before or not, and in the transaction that holds the lock of the table.
 */
void
sift_col_track_deafened(Oid relid)
{
	Relation	heap;

	if (!OidIsValid(relid) || (heap = try_relation_open(relid, NoLock)) == NULL)
		return;
	foreach_oid(ioid, RelationGetIndexList(heap))
	{
		Relation	index = index_open(ioid, RowExclusiveLock);

		if (sift_col_is_index(index) && sift_col_track_on(index))
		{
			SiftTombLog *log = sift_col_log(index);

			if (log != NULL && log->chan.flags != 0)
				sift_tomb_set_chan(index, log, &(SiftTombChan) {0});
		}
		index_close(index, NoLock);
	}
	relation_close(heap, NoLock);
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
	if (sift_col_track_on(index))
	{
		done = channel_as_asked(heap, index, sift_col_log(index));
		table_close(heap, AccessShareLock);
		if (done)
			return;
		heap = table_open(index->rd_index->indrelid, ShareRowExclusiveLock);
		sift_col_track_check(heap, index);
		sift_col_track_install(heap, index, false);
		table_close(heap, NoLock);
		if (sift_col_track_ctid(index))
			ereport(NOTICE, (errmsg("sift_col index \"%s\": track_changes = ctid queues about 42 bytes a changed row", sift_col_final_name(index)),
							 errhint("Before a huge UPDATE or DELETE: ALTER INDEX %s SET (track_changes = 'off'), then 'ctid' again.", sift_col_final_name(index))));
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
 * Did this transaction make the log complete (the baseline of CREATE INDEX, REINDEX or ALTER INDEX)? The baseline tombstones carry its xid and the command that ran it, and the
 * transaction sees them as its own deletes from its next command on, whatever snapshot it reads with: a REPEATABLE READ snapshot taken before the baseline still sees the versions
 * that a transaction committed since had ended (the baseline stamps them dead now), and the log would hide them. The reads of that transaction check the heap.
 */
bool
sift_col_track_own(const SiftTombLog *log)
{
	return log->chan.flags == SIFT_TOMB_CURRENT && TransactionIdIsCurrentTransactionId(log->chan.base_xid);
}

/* The index tracks, the table still has its unique index on the key, and a statement on it fires its triggers: what a log needs to be complete, whoever made it so */
static bool
trackable(Relation heap, Relation index)
{
	SiftColRel *r;

	if (!sift_col_track_on(index))
		return false;
	r = sift_col_rel(index);
	return !(r->rowkey && !OidIsValid(sift_col_row_key_unique(heap, index->rd_index->indkey.values[r->rowkey_attno - 1]))) && sift_col_track_deaf(heap) == NULL;
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

	if (!trackable(heap, index))
		return false;
	if ((log = sift_col_log(index)) == NULL || log->chan.flags != SIFT_TOMB_CURRENT)
		return false;
	channel_read(heap, index, present, ok, witness);
	if (!ok[0] || !ok[1] || witness[0] != log->chan.witness[0] || witness[1] != log->chan.witness[1])
		return false;
	if (snap == NULL)
		return true;
	/* not the transaction that made the log (REINDEX, ALTER INDEX): its snapshot may be older than the baseline, which ends versions that snapshot still sees (sift_col_track_own) */
	return snap->snapshot_type == SNAPSHOT_MVCC && !sift_col_track_own(log) &&
		!XidInMVCCSnapshot(log->chan.base_xid, snap) && sift_xid_state(log->chan.base_xid) == SIFT_XID_COMMITTED;
}

static Oid
trigger_index(Trigger *trig)
{
	return sift_col_track_trigger_index(trig->tgoid);
}

/*
 * An index in service: DROP INDEX CONCURRENTLY marks it dead (indislive) before it waits for the transactions that could still use it, then deletes it holding its lock until it
 * commits. A writer that gets past that mark skips it (it is not read, and a lock on it would wait for the drop, which waits for the writer's lock on the table).
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
rowmemo_inval(Datum arg, Oid relid)
{
	if (relid == InvalidOid || relid == rowmemo.index)
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
		CacheRegisterRelcacheCallback(rowmemo_inval, (Datum) 0);
		rowmemo.hooked = true;
	}
	if (rowmemo.trigger != td->tg_trigger->tgoid)
	{
		rowmemo.index = trigger_index(td->tg_trigger);
		rowmemo.live = index_live(rowmemo.index);
		rowmemo.trigger = td->tg_trigger->tgoid;
	}
	if (!rowmemo.live)
		return PointerGetDatum(NULL);
	index = index_open(rowmemo.index, RowExclusiveLock);
	sift_col_tomb_hold_tid(index, &tid);
	index_close(index, NoLock);
	return PointerGetDatum(NULL);
}

/* sift.col_track_old_rows(): the trigger of the option; its argument is the oid of the index it was made for (trigger_index says which one it serves now). For a key column it is a statement trigger with the old rows, for track_changes = ctid a row trigger */
Datum
sift_col_track_old_rows(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	Relation	index,
				heap;
	AttrNumber	hatt;
	Oid			indexoid;

	if (CALLED_AS_TRIGGER(fcinfo) && TRIGGER_FIRED_AFTER(td->tg_event) && TRIGGER_FIRED_FOR_ROW(td->tg_event) && !TRIGGER_FIRED_BY_INSERT(td->tg_event))
		return track_row(td);
	if (!CALLED_AS_TRIGGER(fcinfo) || !TRIGGER_FIRED_AFTER(td->tg_event) || TRIGGER_FIRED_FOR_ROW(td->tg_event) || td->tg_oldtable == NULL)
		ereport(ERROR, (errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						errmsg("sift.col_track_old_rows is the trigger of an index option: AFTER UPDATE or DELETE, FOR EACH STATEMENT with an old table, or FOR EACH ROW")));
	heap = td->tg_relation;
	indexoid = trigger_index(td->tg_trigger);
	if (!index_live(indexoid))
		return PointerGetDatum(NULL);
	index = index_open(indexoid, RowExclusiveLock);
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
		(heap->trigdesc == NULL || !heap->trigdesc->trig_update_before_row) && RelationGetIndexPredicate(index) == NIL && sift_col_updates_supersede()
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

/* Whether the index tracks changes and its log is not complete: after a concurrent build only the triggers hear, so no read can rely on the log */
bool
sift_col_track_incomplete(Relation index)
{
	SiftTombLog *log = sift_col_track_on(index) ? sift_col_log(index) : NULL;

	return log != NULL && !(log->chan.flags & SIFT_TOMB_COMPLETE);
}

/* sift.col_tracking(index): what the log can be taken for, and which part is missing */
Datum
sift_col_tracking(PG_FUNCTION_ARGS)
{
	Relation	index = sift_col_open_index(PG_GETARG_OID(0), AccessShareLock);
	Relation	heap;
	TupleDesc	desc;
	Datum		v[4];
	bool		nulls[4] = {false};
	SiftTombLog *log;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("function returning record called in context that cannot accept type record"),
						errhint("Call it in the FROM clause: SELECT * FROM sift.col_tracking(index).")));
	SIFT_COL_OPEN_GATE();
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	log = sift_col_track_on(index) ? sift_col_log(index) : NULL;
	v[0] = BoolGetDatum(sift_col_track_on(index));
	v[1] = BoolGetDatum(log != NULL && channel_vouched(heap, index, log));
	v[2] = BoolGetDatum(log != NULL && (log->chan.flags & SIFT_TOMB_COMPLETE));
	v[3] = BoolGetDatum(sift_col_track_ok(index, heap, NULL));
	table_close(heap, NoLock);
	index_close(index, NoLock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, v, nulls)));
}

/* The segments of an index and the xid before which every inserter of their entries committed (NULL: the segment does not say, and reads check the heap for its rows) */
Datum
sift_col_stamps(PG_FUNCTION_ARGS)
{
	Relation	index = sift_col_open_index(PG_GETARG_OID(0), AccessShareLock);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));

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
