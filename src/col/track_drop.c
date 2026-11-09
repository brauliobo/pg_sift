/*
 * col/track_drop.c - the dependency between a sift_col index and its two track_changes triggers, and what removes them with the index (the lifecycle of the triggers is in track.c).
 *
 * The index depends on its triggers, and they do not depend on it: DROP INDEX CONCURRENTLY refuses to start once a transaction id is assigned, and deletes what depends on the index
 * first. An index nothing depends on drops, and the transaction that deletes it removes the triggers (gone_xact). The triggers of an index of an earlier release depend on the index (until
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
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "col/am.h"

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

/* The oids of the triggers of track_changes in the database */
static List *
our_triggers(Oid funcoid)
{
	Relation	trel = table_open(TriggerRelationId, AccessShareLock);
	SysScanDesc scan = systable_beginscan(trel, InvalidOid, false, NULL, 0, NULL);
	HeapTuple	tp;
	List	   *l = NIL;

	while (HeapTupleIsValid(tp = systable_getnext(scan)))
		if (((Form_pg_trigger) GETSTRUCT(tp))->tgfoid == funcoid)
			l = lappend_oid(l, ((Form_pg_trigger) GETSTRUCT(tp))->oid);
	systable_endscan(scan);
	table_close(trel, AccessShareLock);
	return l;
}

/*
 * The triggers of an index that is deleted go with it, in the transaction that deletes it: no index depends on them any more, and this transaction removes them before it commits
 * (DROP INDEX CONCURRENTLY, DROP INDEX, ALTER TABLE .. DROP COLUMN, DROP OWNED; a table or schema that goes takes them with it). The statement that deletes the index cannot do it:
 * DROP INDEX CONCURRENTLY must find no transaction id yet, and deletes what depends on the index first. The relcache invalidation of the deleted index is the notice, and the
 * transaction asks the catalog about each invalidated relation just before it commits, once it has an xid (a transaction that deletes has).
 */
static Oid *gone;
static int	ngone;
static int	gone_cap;

static void
gone_inval(Datum arg, Oid relid)
{
	if (relid == InvalidOid || !TransactionIdIsValid(GetTopTransactionIdIfAny()))
		return;
	if (ngone == gone_cap)
	{
		gone_cap = gone_cap ? gone_cap * 2 : 16;
		gone = gone ? repalloc(gone, gone_cap * sizeof(Oid)) : MemoryContextAlloc(TopMemoryContext, gone_cap * sizeof(Oid));
	}
	gone[ngone++] = relid;
}

static void
remove_orphans(Oid funcoid)
{
	List	   *orphans = NIL;

	foreach_oid(t, our_triggers(funcoid))
		if (!OidIsValid(depends_index(t)) && !OidIsValid(legacy_index(t)))
			orphans = lappend_oid(orphans, t);
	foreach_oid(t, orphans)
		sift_col_track_delete_trigger(t);
}

static void
gone_xact(XactEvent event, void *arg)
{
	bool		deleted = false;
	Oid			funcoid;

	if (event == XACT_EVENT_ABORT)
		ngone = 0;
	if ((event != XACT_EVENT_PRE_COMMIT && event != XACT_EVENT_PRE_PREPARE) || ngone == 0)
		return;
	for (int i = 0; i < ngone && !deleted; i++)
		deleted = !SearchSysCacheExists1(RELOID, ObjectIdGetDatum(gone[i]));
	ngone = 0;
	/* no function, no trigger of ours (an extension that is not at the release that has it yet) */
	if (!deleted || RecoveryInProgress() || !OidIsValid(funcoid = sift_col_track_function(true)))
		return;
	remove_orphans(funcoid);
	CommandCounterIncrement();
	ngone = 0;
}

/* Called by the handler of the access method, which every session that opens a sift_col index runs before the index can be deleted */
void
sift_col_track_hook(void)
{
	static bool hooked = false;

	if (hooked)
		return;
	hooked = true;
	RegisterXactCallback(gone_xact, NULL);
	CacheRegisterRelcacheCallback(gone_inval, (Datum) 0);
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
	foreach_oid(t, our_triggers(sift_col_track_function(false)))
	{
		Oid			index = legacy_index(t);

		if (OidIsValid(index))
		{
			ObjectAddress trig,
						idx;

			deleteDependencyRecordsForSpecific(TriggerRelationId, t, DEPENDENCY_AUTO, RelationRelationId, index);
			ObjectAddressSet(trig, TriggerRelationId, t);
			ObjectAddressSet(idx, RelationRelationId, index);
			recordDependencyOn(&idx, &trig, DEPENDENCY_NORMAL);
			n++;
		}
	}
	PG_RETURN_INT32(n);
}

