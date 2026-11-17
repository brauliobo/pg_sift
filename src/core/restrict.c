/*
 * core/restrict.c - the contexts in which sift may not write the catalog (see core/restrict.h).
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "storage/proc.h"

#include "core/compat.h"
#include "core/restrict.h"
#include "core/sift.h"

bool
sift_in_vacuum(void)
{
	return (MyProc->statusFlags & PROC_IN_VACUUM) != 0;
}

bool
sift_xid_unassignable(void)
{
	return IsInParallelMode() && !FullTransactionIdIsValid(GetTopFullTransactionIdIfAny());
}

bool
sift_catalog_write_ok(void)
{
	return !IsInParallelMode() && !sift_in_vacuum();
}

void
sift_catalog_write_guard(const char *what)
{
	SIFT_INVARIANT(sift_catalog_write_ok(), "%s writes a catalog row in a VACUUM or a parallel operation", what);
}

void
sift_cat_update(Relation catalog, HeapTuple tup)
{
	sift_catalog_write_guard("a catalog update");
	CatalogTupleUpdate(catalog, &tup->t_self, tup);
}

void
sift_cat_insert(Relation catalog, HeapTuple tup)
{
	sift_catalog_write_guard("a catalog insert");
	CatalogTupleInsert(catalog, tup);
}

void
sift_cat_delete(Relation catalog, ItemPointer tid)
{
	sift_catalog_write_guard("a catalog delete");
	CatalogTupleDelete(catalog, tid);
}

void
sift_depend(const ObjectAddress *dependent, const ObjectAddress *referenced, DependencyType type)
{
	sift_catalog_write_guard("a dependency");
	recordDependencyOn(dependent, referenced, type);
}

void
sift_undepend(Oid classId, Oid objectId, DependencyType deptype, Oid refClassId, Oid refObjectId)
{
	sift_catalog_write_guard("a dependency");
	deleteDependencyRecordsForSpecific(classId, objectId, deptype, refClassId, refObjectId);
}

ObjectAddress
sift_create_trigger(CreateTrigStmt *stmt, Oid relid, Oid funcoid)
{
	sift_catalog_write_guard("a trigger");
	return CreateTriggerFiringOn(stmt, "", relid, InvalidOid, InvalidOid, InvalidOid, funcoid, InvalidOid, NULL, true, false, TRIGGER_FIRES_ALWAYS);
}

void
sift_drop_object(const ObjectAddress *object)
{
	sift_catalog_write_guard("a drop");
	performDeletion(object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
}

ObjectAddress
sift_create_index(Oid relid, IndexStmt *stmt)
{
	sift_catalog_write_guard("an index");
	return sift_define_index(relid, stmt);
}

void
sift_index_set_state(Oid index, IndexStateFlagsAction action)
{
	sift_catalog_write_guard("an index state");
	index_set_state_flags(index, action);
}

Oid
sift_create_matview(const char *name, Oid namespaceId, Oid ownerId, Oid accessMethod, TupleDesc tupdesc, char persistence)
{
	sift_catalog_write_guard("a relation");
	return heap_create_with_catalog(name, namespaceId, InvalidOid, InvalidOid, InvalidOid, InvalidOid, ownerId, accessMethod, tupdesc, NIL, RELKIND_MATVIEW,
									persistence, false, false, ONCOMMIT_NOOP, (Datum) 0, false, false, true, InvalidOid, NULL);
}

void
sift_change_owner(Oid relid, Oid ownerId)
{
	sift_catalog_write_guard("an owner");
	ATExecChangeOwner(relid, ownerId, false, AccessExclusiveLock);
}
