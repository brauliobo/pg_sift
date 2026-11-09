#!/usr/bin/env bash
# A sift_table against a plain heap twin under concurrent writers, readers and maintenance (test/fuzz/tbl_twin_fuzz.py): the same statements go to both in one
# transaction, VACUUMs queue behind a held maintenance lock while the writers close write buffers and the holder promotes (the order in which a VACUUM that had
# waited for the lock acted on a list of segments it had read before it, and old versions of updated rows were live again), and after every round the rows of the
# two must be the same through every plan, in a long snapshot and in a fresh one. Quick tier: seeds 2 and 3 of 8 s with the lock held in every round, about a
# minute; full: 24 seeds of 80 s. A seed that fails on the code before the fix is a probability, see the header of the script. SIFT_FUZZ_SEEDS / SIFT_FUZZ_SECONDS override.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

SEEDS=$(knob SIFT_FUZZ_SEEDS 1-24 2-3)
SECONDS_PER_SEED=$(knob SIFT_FUZZ_SECONDS 80 8)
HOLD=$(knob SIFT_FUZZ_HOLD 0.5 1)

node_init tw 36 "autovacuum = on
autovacuum_naptime = 2s
sift.maintenance_naptime = 200
sift.table_auto_defrag = on
sift.async_maintenance = on
max_wal_size = 512MB
shared_buffers = 128MB
max_locks_per_transaction = 256"
node_start tw || bail "start failed"
sql tw "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions failed"
plan 1

out=$(PGHOST=$(dir tw)/sock PGPORT=$(port tw) PGUSER=postgres SIFT_FUZZ_OUT=$(dir tw)/fuzz \
	python3 -I "$REPO/test/fuzz/tbl_twin_fuzz.py" --seeds "$SEEDS" --seconds "$SECONDS_PER_SEED" --hold "$HOLD" 2>&1)
rc=$?
for l in $(grep -o 'log=.*' <<<"$out" | cut -d= -f2); do
	grep -q 'FAIL\|HARNESS' <<<"$(grep "log=$l" <<<"$out")" && diag "$(tail -n 25 "$l" | cut -c1-300)"
done
check "the sift_table and its heap twin agree in every seed ($SEEDS)" test "$rc" == 0
finish
