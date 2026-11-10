#!/usr/bin/env bash
# The order arrays of a sift_col index against its b-tree and heap twins under churn (test/fuzz/col_twin_fuzz.py): INSERT, DELETE, UPDATE (of a key and not), upsert,
# an aborted transaction, repeated updates of one row, promotion, VACUUM, merge and REINDEX in a random order, with track_changes on and off, seekable chunks of the build's
# choice and all of them, and a long snapshot reader; after every step the ordered rows of top-N, OFFSET, range, IN list, group and NULL-region queries must be the same
# through the forced sift plan, the planned one, a b-tree and the heap. Quick tier: seeds 1-2 of 6 steps on 12,000 rows; full: 24 seeds of 10 steps on 30,000.
# SIFT_FUZZ_SEEDS / SIFT_FUZZ_ROWS / SIFT_FUZZ_STEPS override.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SEEDS=$(knob SIFT_FUZZ_SEEDS 1-24 1-2)
ROWS=$(knob SIFT_FUZZ_ROWS 30000 12000)
STEPS=$(knob SIFT_FUZZ_STEPS 10 6)

node_init cf 5 "max_wal_size = 512MB
shared_buffers = 128MB
max_locks_per_transaction = 256"
node_start cf || bail "start failed"
sql cf "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions failed"
plan 1

out=$(PGHOST=$(dir cf)/sock PGPORT=$(port cf) PGUSER=postgres SIFT_FUZZ_OUT=$(dir cf)/fuzz \
	python3 -I "$REPO/test/fuzz/col_twin_fuzz.py" --seeds "$SEEDS" --rows "$ROWS" --steps "$STEPS" 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the order arrays and their b-tree and heap twins agree in every seed ($SEEDS)" test "$rc" == 0
finish
