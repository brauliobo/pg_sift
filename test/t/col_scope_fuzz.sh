#!/usr/bin/env bash
# The scope index of a sift_col index against the plain join under churn (test/fuzz/col_scope_fuzz.py): fact INSERTs, DELETEs, UPDATEs (also of the entity), upserts, pair INSERTs, DELETEs, moves of an
# entity between keys (entities in 0 to 6 keys), UPDATEs of the key of a pair, a reload of the pairs, aborted transactions and savepoints, promotion, merge, VACUUM and REINDEX in a random order, with a
# long snapshot reader; after every step a battery (one key, sets of keys counted once, per pair, count/sum/distinct/top-N, the SQL forms) must be the plain join through the scope path, the planned plan
# and the plain plan, and every read is either stored or says a documented fallback reason. The planner forms are probed by the
# script (the planner's recognition of the join forms is not in every tree). Quick tier: seeds 1-2 of 5 steps on 6,000 rows; full: 24 seeds of 8 steps on 12,000 (about 3 minutes).
# Every third seed is a union seed (the scope of a direct and a joined path: grants of a member, grants of a group, memberships; see the docstring of the script for --twohop-strict).
# SIFT_FUZZ_SEEDS / SIFT_FUZZ_ROWS / SIFT_FUZZ_STEPS override.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SEEDS=$(knob SIFT_FUZZ_SEEDS 1-24 1-2)
ROWS=$(knob SIFT_FUZZ_ROWS 12000 6000)
STEPS=$(knob SIFT_FUZZ_STEPS 8 5)

node_init sf 5 "max_wal_size = 512MB
shared_buffers = 128MB
max_locks_per_transaction = 256"
node_start sf || bail "start failed"
sql sf "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions failed"
plan 1

out=$(PGHOST=$(dir sf)/sock PGPORT=$(port sf) PGUSER=postgres SIFT_FUZZ_OUT=$(dir sf)/fuzz \
	python3 -I "$REPO/test/fuzz/col_scope_fuzz.py" --seeds "$SEEDS" --rows "$ROWS" --steps "$STEPS" 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the scope reads are the plain join in every seed ($SEEDS)" test "$rc" == 0
finish
