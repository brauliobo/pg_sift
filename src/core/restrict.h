/*
 * core/restrict.h - the contexts in which a backend may not write the catalog, and the only functions of sift that do.
 *
 * A lazy VACUUM (and an autovacuum worker) runs with PROC_IN_VACUUM: GetSnapshotData() leaves its transaction id out of every snapshot, so a session that reads a row the VACUUM
 * wrote takes the VACUUM's xid for an aborted writer, writes HEAP_XMAX_INVALID on the old version, and the row is there twice (or its update is lost) once the VACUUM commits.
 * A parallel operation cannot assign a transaction id at all. CREATE INDEX CONCURRENTLY is not restricted: its xid is in the snapshots, and a build chooses options.
 *
 * sift never writes a catalog row in those contexts. Every write goes through a function below, which raises an error (an assertion with cassert) there, so the rule is the code and
 * not something to remember; scripts/check-catalog-writes.sh fails a file of src/ that calls the PostgreSQL function directly. A decision that a VACUUM reaches and that ends in
 * a catalog write asks sift_catalog_write_ok() and leaves the decision to the maintenance worker. To add a writer: add a function here that calls sift_catalog_write_guard first.
 */
#ifndef SIFT_RESTRICT_H
#define SIFT_RESTRICT_H

#include "catalog/dependency.h"
#include "nodes/parsenodes.h"
#include "utils/rel.h"

/* Does this backend run a lazy VACUUM (its transaction id is left out of snapshots and of the horizon)? */
extern bool sift_in_vacuum(void);

/* True where this backend cannot assign a transaction id: the leader of a parallel VACUUM runs the cleanup of the indexes that have no parallel pass in parallel mode */
extern bool sift_xid_unassignable(void);

/* May this backend write a catalog row: not in a VACUUM, not in a parallel operation */
extern bool sift_catalog_write_ok(void);
extern void sift_catalog_write_guard(const char *what);

/* The writers: CatalogTupleUpdate/Insert/Delete (tup: the new version, tid: the row), recordDependencyOn, deleteDependencyRecordsForSpecific, CreateTrigger, performDeletion (restrict, internal) */
extern void sift_cat_update(Relation catalog, HeapTuple tup);
extern void sift_cat_insert(Relation catalog, HeapTuple tup);
extern void sift_cat_delete(Relation catalog, ItemPointer tid);
extern void sift_depend(const ObjectAddress *dependent, const ObjectAddress *referenced, DependencyType type);
extern void sift_undepend(Oid classId, Oid objectId, DependencyType deptype, Oid refClassId, Oid refObjectId);
extern ObjectAddress sift_create_trigger(CreateTrigStmt *stmt, Oid relid, Oid funcoid);
extern void sift_drop_object(const ObjectAddress *object);

#endif
