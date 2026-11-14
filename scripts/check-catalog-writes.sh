#!/usr/bin/env bash
# A file of src/ that writes a catalog row (CatalogTupleInsert/Update/Delete, a dependency, a trigger, a deletion of an object, a relation or an index it makes) calls
# sift_catalog_write_guard (core/store.c): a VACUUM runs with PROC_IN_VACUUM, so its transaction id is left out of every snapshot and a session that reads a row the VACUUM wrote
# takes it for an aborted writer and hints the old version as not updated. The guard makes a writer that a VACUUM (or a parallel operation) reaches an error, not a damaged
# catalog; what the VACUUM has to decide is left to the maintenance worker (docs/TUNING.md).
#
#   scripts/check-catalog-writes.sh
set -euo pipefail
cd "$(dirname "$0")/.."
bad=$(for f in $(git grep -lE 'CatalogTuple(Insert|Update|Delete)\(|recordDependencyOn\(|deleteDependencyRecords[A-Za-z]*\(|CreateTrigger[A-Za-z]*\(|performDeletion\(|heap_create_with_catalog\(|sift_define_index\(' -- src ':!src/core/compat.h' ':!src/core/store.c'); do
	grep -q 'sift_catalog_write_guard(' "$f" || echo "$f"
done)
[[ -z $bad ]] || { echo "check-catalog-writes: these files write a catalog row without sift_catalog_write_guard:"; echo "$bad"; exit 1; }
echo "check-catalog-writes: ok"
