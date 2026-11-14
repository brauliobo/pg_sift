/*
 * col/track_drop.c - the dependency between a sift_col index and its two track_changes triggers, and what removes them with the index (the lifecycle of the triggers is in track.c).
 *
 * The index depends on its triggers, and they do not depend on it: DROP INDEX CONCURRENTLY refuses to start once a transaction id is assigned, and deletes what depends on the index
 * first. An index nothing depends on drops, and the transaction that deletes it removes the triggers (gone_xact), or a worker does when the table is in use (remove_orphans). The triggers of an index of an earlier release depend on the index (until
 * ALTER EXTENSION UPDATE runs sift.col_track_convert()), and are found in that form too: the library does not wait for the update to know its own triggers. REINDEX CONCURRENTLY swaps the dependencies of the old and the
 * new index, so the triggers follow the index that stays.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/dependency.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_trigger.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "col/am.h"
#include "core/visibility.h"

PG_FUNCTION_INFO_V1(sift_col_track_convert);

void
sift_col_track_delete_trigger(Oid trigger)
{
	ObjectAddress obj;

	ObjectAddressSet(obj, TriggerRelationId, trigger);
	performDeletion(&obj, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
}

/*
 * The index a trigger serves: the one that depends on it. REINDEX CONCURRENTLY swaps what depends on the old index and on the new one before it drops the old, so the oid in the arguments (the one the trigger was made for)
 * names an index that is gone afterwards.
 */
static Oid
depends_index(Oid trigger)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			index = InvalidOid;

	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(TriggerRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(trigger));
	scan = systable_beginscan(drel, DependReferenceIndexId, true, NULL, 2, key);
	while (!OidIsValid(index) && HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->classid == RelationRelationId && d->deptype == DEPENDENCY_NORMAL && d->refobjsubid == 0 && get_rel_relkind(d->objid) == RELKIND_INDEX)
			index = d->objid;
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	return index;
}

/* The index that an earlier release made the trigger the dependent of: the library that is not yet followed by ALTER EXTENSION UPDATE finds its indexes so (sift.col_track_convert) */
static Oid
legacy_index(Oid trigger)
{
	Relation	drel = table_open(DependRelationId, AccessShareLock);
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			index = InvalidOid;

	ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(TriggerRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(trigger));
	scan = systable_beginscan(drel, DependDependerIndexId, true, NULL, 2, key);
	while (!OidIsValid(index) && HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend d = (Form_pg_depend) GETSTRUCT(tup);

		if (d->refclassid == RelationRelationId && d->deptype == DEPENDENCY_AUTO && d->refobjsubid == 0 && get_rel_relkind(d->refobjid) == RELKIND_INDEX)
			index = d->refobjid;
	}
	systable_endscan(scan);
	table_close(drel, AccessShareLock);
	return index;
}

/*
 * The index a trigger serves: the one that depends on it, or the one an earlier release made it the dependent of (until ALTER EXTENSION UPDATE converts it). InvalidOid for a trigger
 * whose index is deleted: the transaction that deletes an index removes its triggers just before it commits, so its own later statements and the ones that run beside it fire a trigger with
 * no index behind it, and have nothing to do.
 */
Oid
sift_col_track_trigger_index(Oid trigger)
{
	Oid			index = depends_index(trigger);

	return OidIsValid(index) ? index : legacy_index(trigger);
}

typedef struct TrackTrigger
{
	Oid			oid;
	Oid			relid;			/* the table */
} TrackTrigger;

/* The triggers of track_changes in the database */
static List *
our_triggers(Oid funcoid)
{
	Relation	trel = table_open(TriggerRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(trel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tp;
	List	   *l = NIL;

	while (HeapTupleIsValid(tp = systable_getnext(scan)))
	{
		Form_pg_trigger tg = (Form_pg_trigger) GETSTRUCT(tp);
		TrackTrigger *t;

		if (tg->tgfoid != funcoid)
			continue;
		t = palloc(sizeof(TrackTrigger));
		*t = (TrackTrigger) {.oid = tg->oid,.relid = tg->tgrelid};
		l = lappend(l, t);
	}
	systable_endscan(scan);
	table_close(trel, AccessShareLock);
	return l;
}

/* Those that no index serves: their index is deleted */
static List *
orphan_triggers(Oid funcoid)
{
	List	   *orphans = NIL;

	foreach_ptr(TrackTrigger, t, our_triggers(funcoid))
		if (!OidIsValid(sift_col_track_trigger_index(t->oid)))
			orphans = lappend(orphans, t);
	return orphans;
}

/*
 * The triggers of an index that is deleted go with it, in the transaction that deletes it: no index depends on them any more, and this transaction removes them before it commits
 * (DROP INDEX CONCURRENTLY, DROP INDEX, ALTER TABLE .. DROP COLUMN, DROP OWNED; a table or schema that goes takes them with it). The statement that deletes the index cannot do it:
 * DROP INDEX CONCURRENTLY must find no transaction id yet, and deletes what depends on the index first. The object access hook (sift_object_access) names each sift_col index
 * that is deleted, and the transaction asks the catalog about them just before it commits, once it has an xid (a transaction that deletes has). The library that a session loads
 * while it deletes its first index (the access method handler runs inside the drop) was not there for the hook of that index: the transactions of the statement that loaded it look for the orphans (DROP INDEX CONCURRENTLY deletes in the last of four).
 * No relcache callback does this: a backend has ten slots of them for all its libraries (scripts/check-relcache-slots.sh).
 */
static Oid *gone;
static int	ngone;
static int	gone_cap;
static TimestampTz loaded_stmt;	/* the statement that loaded the library, 0: none or ended */

void
sift_col_track_dropping(Oid relid)
{
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	bool		ours;

	if (!HeapTupleIsValid(tp))
		return;
	ours = ((Form_pg_class) GETSTRUCT(tp))->relkind == RELKIND_INDEX && ((Form_pg_class) GETSTRUCT(tp))->relam == sift_col_am_oid();
	ReleaseSysCache(tp);
	if (!ours)
		return;
	if (ngone == gone_cap)
	{
		gone_cap = gone_cap ? gone_cap * 2 : 16;
		gone = gone ? repalloc(gone, gone_cap * sizeof(Oid)) : MemoryContextAlloc(TopMemoryContext, gone_cap * sizeof(Oid));
	}
	gone[ngone++] = relid;
}

/*
 * The libraries' background worker (sift_col_track_orphans_main) removes what the transaction could not: DROP TRIGGER takes the ACCESS EXCLUSIVE lock of the table, which waits for every
 * statement that holds it and keeps every new one waiting behind it, so a DROP INDEX CONCURRENTLY that took it would stop the table for as long as its longest reader (it waited for
 * those that began before it marked the index dead, and no longer). The transaction takes the lock only if it is free this instant (the table is often idle, that drop has just waited for it);
 * otherwise the triggers stay, with no index behind them (they do nothing, trigger_index), and the worker takes the lock for no longer than a moment at a time, until it has it.
 */
#define ORPHAN_LOCK_TIMEOUT_MS	"20"
#define ORPHAN_ROUNDS			300
#define ORPHAN_NAP_MS			1000

static void
ask_orphans(TransactionId xid)
{
	BackgroundWorker bw = {0};
	BackgroundWorkerHandle *handle;

	if (!IsUnderPostmaster)
		return;
	bw.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bw.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_sift");
	snprintf(bw.bgw_function_name, BGW_MAXLEN, "sift_col_track_orphans_main");
	snprintf(bw.bgw_name, BGW_MAXLEN, "sift track triggers (database %u)", MyDatabaseId);
	snprintf(bw.bgw_type, BGW_MAXLEN, "sift maintenance");
	bw.bgw_main_arg = TransactionIdGetDatum(xid);
	memcpy(bw.bgw_extra, &MyDatabaseId, sizeof(Oid));
	if (!RegisterDynamicBackgroundWorker(&bw, &handle))
		ereport(LOG, (errmsg("sift: no background worker is free to remove the track_changes triggers of a dropped index"),
					  errdetail("They do nothing; the next statement of their table that fires one asks again.")));
}

/* A trigger whose index is gone fired: the worker may not have run (or given up on a busy table), once for every nap of the maintenance worker */
void
sift_col_track_orphan_fired(void)
{
	static TimestampTz asked_at = 0;
	TimestampTz now = GetCurrentTimestamp();

	if (RecoveryInProgress() || now - asked_at < (int64) sift_maintenance_naptime * 1000)
		return;
	asked_at = now;
	ask_orphans(InvalidTransactionId);
}

static void
remove_orphans(Oid funcoid)
{
	bool		later = false;

	foreach_ptr(TrackTrigger, t, orphan_triggers(funcoid))
	{
		if (ConditionalLockRelationOid(t->relid, AccessExclusiveLock))
			sift_col_track_delete_trigger(t->oid);
		else
			later = true;
	}
	CommandCounterIncrement();
	if (later)
		ask_orphans(GetTopTransactionId());
}

static void
gone_xact(XactEvent event, void *arg)
{
	bool		deleted = false;
	Oid			funcoid;

	int			kept = 0;
	bool		loading;

	if (event == XACT_EVENT_ABORT)
		ngone = 0;
	if (loaded_stmt != 0 && GetCurrentStatementStartTimestamp() != loaded_stmt)
		loaded_stmt = 0;
	loading = loaded_stmt != 0 && TransactionIdIsValid(GetTopTransactionIdIfAny());
	if ((event != XACT_EVENT_PRE_COMMIT && event != XACT_EVENT_PRE_PREPARE) || (ngone == 0 && !loading))
		return;
	/* DROP INDEX CONCURRENTLY names its index in the first of its transactions and deletes it in the last: the ones still there wait for the next */
	for (int i = 0; i < ngone; i++)
		if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(gone[i])))
			gone[kept++] = gone[i];
		else
			deleted = true;
	ngone = kept;
	/* no function, no trigger of ours (an extension that is not at the release that has it yet) */
	if ((!deleted && !loading) || RecoveryInProgress() || !OidIsValid(funcoid = sift_col_track_function(true)))
		return;
	/* the hook did not see the deletes of the statement that loaded the library: the triggers with no index tell */
	if (!deleted && orphan_triggers(funcoid) == NIL)
		return;
	SIFT_TEST_GATE(sift_test_drop_gate, 9);
	remove_orphans(funcoid);
	CommandCounterIncrement();
}

/* Called when the library loads: a transaction that loads it may be the one that deletes an index, which the hook did not see */
void
sift_col_track_init(void)
{
	RegisterXactCallback(gone_xact, NULL);
	loaded_stmt = IsTransactionState() ? GetCurrentStatementStartTimestamp() : 0;
}

/* Does the trigger still serve no index (it is looked at again under the lock of its table) */
static bool
still_orphan(Oid trigger, Oid funcoid)
{
	foreach_ptr(TrackTrigger, t, our_triggers(funcoid))
		if (t->oid == trigger)
			return !OidIsValid(sift_col_track_trigger_index(trigger));
	return false;
}

/* One trigger, in a transaction of its own; false when the table stayed busy for the lock timeout */
static bool
drop_orphan(const TrackTrigger *t, Oid funcoid, MemoryContext cx)
{
	volatile bool done = true;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	PG_TRY();
	{
		LockRelationOid(t->relid, AccessExclusiveLock);
		if (still_orphan(t->oid, funcoid))
			sift_col_track_delete_trigger(t->oid);
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *err;

		MemoryContextSwitchTo(cx);
		err = CopyErrorData();
		if (err->sqlerrcode != ERRCODE_LOCK_NOT_AVAILABLE)
			PG_RE_THROW();
		FlushErrorState();
		FreeErrorData(err);
		AbortCurrentTransaction();
		done = false;
	}
	PG_END_TRY();
	return done;
}

/* One pass over the orphans; true when none is left */
static bool
drop_orphans(Oid funcoid, MemoryContext cx)
{
	List	   *orphans;
	bool		all = true;

	StartTransactionCommand();
	MemoryContextSwitchTo(cx);
	orphans = orphan_triggers(funcoid);
	CommitTransactionCommand();
	MemoryContextSwitchTo(cx);
	foreach_ptr(TrackTrigger, t, orphans)
		all &= drop_orphan(t, funcoid, cx);
	return all;
}

/*
 * The removal of the triggers that the transaction of a DROP INDEX CONCURRENTLY left (remove_orphans). main_arg: the transaction to wait for, or none (a statement asked). One worker at a time
 * in a database: a worker asked by a statement leaves if another one is there, one asked by a drop waits for it, since the other may have looked before that drop committed.
 */
void
sift_col_track_orphans_main(Datum main_arg)
{
	TransactionId xid = DatumGetTransactionId(main_arg);
	MemoryContext cx = CurrentMemoryContext;
	LOCKTAG		tag;
	Oid			dboid;
	Oid			funcoid;

	memcpy(&dboid, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnectionByOid(dboid, InvalidOid, 0);
	SetConfigOption("lock_timeout", ORPHAN_LOCK_TIMEOUT_MS, PGC_SUSET, PGC_S_SESSION);
	StartTransactionCommand();
	if (TransactionIdIsValid(xid))
		XactLockTableWait(xid, NULL, NULL, XLTW_None);
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F8, 1, 2);
	if (LockAcquire(&tag, ExclusiveLock, true, !TransactionIdIsValid(xid)) == LOCKACQUIRE_NOT_AVAIL)
	{
		CommitTransactionCommand();
		return;
	}
	funcoid = sift_col_track_function(true);
	CommitTransactionCommand();
	MemoryContextSwitchTo(cx);
	for (int round = 0; OidIsValid(funcoid) && round < ORPHAN_ROUNDS && !drop_orphans(funcoid, cx); round++)
	{
		sift_nap(ORPHAN_NAP_MS);
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * sift.col_track_convert(): the update to 4.2.1 makes the index the dependent of its two triggers (they were the dependents of the index, which DROP INDEX CONCURRENTLY could not
 * drop). Returns the number of triggers converted; a second run finds none.
 */
Datum
sift_col_track_convert(PG_FUNCTION_ARGS)
{
	int			n = 0;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("must be superuser to convert the triggers of track_changes")));
	foreach_ptr(TrackTrigger, t, our_triggers(sift_col_track_function(false)))
	{
		Oid			index;

		/* the lock of the DDL that deletes or rebuilds an index (DROP INDEX with or without CONCURRENTLY, REINDEX): it has finished or is waited for, and what it deleted is not read below */
		LockRelationOid(t->relid, ShareUpdateExclusiveLock);
		index = legacy_index(t->oid);

		if (OidIsValid(index))
		{
			ObjectAddress trig,
						idx;

			deleteDependencyRecordsForSpecific(TriggerRelationId, t->oid, DEPENDENCY_AUTO, RelationRelationId, index);
			ObjectAddressSet(trig, TriggerRelationId, t->oid);
			ObjectAddressSet(idx, RelationRelationId, index);
			recordDependencyOn(&idx, &trig, DEPENDENCY_NORMAL);
			n++;
		}
	}
	PG_RETURN_INT32(n);
}

