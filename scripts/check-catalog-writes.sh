#!/usr/bin/env bash
# sift never writes a catalog row in a VACUUM or a parallel operation (core/restrict.h): a VACUUM runs with PROC_IN_VACUUM, so its transaction id is left out of every snapshot and a
# session that reads a row the VACUUM wrote takes it for an aborted writer and hints the old version as not updated. Every write goes through a function of core/restrict.c, which
# raises an error there; a file of src/ that calls the PostgreSQL function itself (a catalog tuple, a dependency, a trigger, the drop or the making of an object) fails here.
# vac_update_relstats is not in the list: it updates in place, takes no transaction id, and is what PostgreSQL itself does in a VACUUM.
#
#   scripts/check-catalog-writes.sh
set -euo pipefail
cd "$(dirname "$0")/.."
direct='\b(CatalogTuple(Insert|Update|Delete|Truncate)|heap_inplace_update|simple_heap_(insert|update|delete)|recordDependencyOn|recordMultipleDependencies|deleteDependencyRecords[A-Za-z]*|changeDependencyFor|CreateTrigger[A-Za-z]*|RemoveTriggerById|performDeletion|performMultipleDeletions|index_drop|index_create|heap_create_with_catalog|heap_drop_with_catalog|DefineIndex|sift_define_index|ATExec[A-Za-z]*|RenameRelation|SetRelationHasSubclass)\('
bad=$(git grep -nE "$direct" -- src ':!src/core/restrict.c' ':!src/core/compat.h' | grep -vE '^[^:]+:[0-9]+:\s*(/?\*|//)' || true)
[[ -z $bad ]] || { echo "check-catalog-writes: these lines write the catalog without core/restrict.c (add a writer there that calls sift_catalog_write_guard):"; echo "$bad"; exit 1; }
echo "check-catalog-writes: ok"
