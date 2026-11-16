#!/usr/bin/env bash
# PostgreSQL forbids every other heavyweight lock while the relation extension lock is held, and whatever runs under it may report an error whose context and damage record look
# names up (core/extlock.h). The lock is taken, and a relation extended under it, only in core/extlock.c, which makes those lookups first; a file of src/ that calls the PostgreSQL
# function itself fails here.
#
#   scripts/check-extlock.sh
set -euo pipefail
cd "$(dirname "$0")/.."
direct='\b(LockRelationForExtension|ConditionalLockRelationForExtension|ExtendBufferedRel[A-Za-z]*|RelationAddBlocks)\('
bad=$(git grep -nE "$direct" -- src ':!src/core/extlock.c' ':!src/core/compat.h' | grep -vE '^[^:]+:[0-9]+:\s*(/?\*|//)' || true)
[[ -z $bad ]] || { echo "check-extlock: these lines take the extension lock or extend a relation without core/extlock.c (call sift_extension_lock / sift_extend_zeroed_to there: they make the lookups of a report before the lock, and a lookup under it is a lock-order violation, not a thing to guard at the call site):"; echo "$bad"; exit 1; }
echo "check-extlock: ok"
