#!/usr/bin/env bash
# The order arrays of a sift_col index against its b-tree and heap twins under churn (test/fuzz/col_twin_fuzz.py): INSERT, DELETE, UPDATE (of a key and not), upsert,
# an aborted transaction, repeated updates of one row, promotion, VACUUM, sift.maintain (the fold of the marks of the log on Format 1), merge and REINDEX in a random order, with track_changes on and off,
# and a long snapshot reader; after every step the ordered rows of top-N, OFFSET, range, IN list, group and NULL-region queries must be the same
# through the forced sift plan, the planned one, a b-tree and the heap, and sift.check must find nothing.
# One script for every job: SIFT_SEEDS (default 1-12 of 10 steps on 30,000 rows; quick tier 1-2 of 6 steps on 12,000), SIFT_FUZZ_ROWS, SIFT_FUZZ_STEPS and SIFT_TEST_FORMAT (f2, f1 or
# release: the format of the library under test, default f2) select the run; the full tier runs the 24 seeds as two jobs of 12 (SIFT_SEEDS=1-12, 13-24: 24 seeds took 1350 s on a hosted runner).
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SEEDS=$(knob SIFT_SEEDS 1-12 1-2)
ROWS=$(knob SIFT_FUZZ_ROWS 30000 12000)
STEPS=$(knob SIFT_FUZZ_STEPS 10 6)
FORMAT=${SIFT_TEST_FORMAT:-f2}

node_init cf 5 "max_wal_size = 512MB
shared_buffers = 128MB
max_locks_per_transaction = 256"
node_start cf || bail "start failed"
sql cf "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
[[ $FORMAT == release ]] || sql cf "CREATE EXTENSION pg_sift_test" >/dev/null || bail "test extension failed"
plan 1

out=$(PGHOST=$(dir cf)/sock PGPORT=$(port cf) PGUSER=postgres SIFT_FUZZ_OUT=$(dir cf)/fuzz \
	python3 -I "$REPO/test/fuzz/col_twin_fuzz.py" --format "$FORMAT" --seeds "$SEEDS" --rows "$ROWS" --steps "$STEPS" 2>&1)
rc=$?
diag "$(grep -E 'FAIL|^seed=' <<<"$out" | cut -c1-400)"
check "the order arrays and their b-tree and heap twins agree in every seed ($SEEDS, $FORMAT)" test "$rc" == 0
finish
