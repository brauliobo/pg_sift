#!/usr/bin/env bash
# A sift_table against a plain heap twin under concurrent writers, readers, maintenance and DDL (test/fuzz/tbl_twin.py): the same statements go to both in one transaction,
# VACUUM, sift.maintain, VACUUM FULL, REINDEX and ALTER TABLE (ADD / DROP COLUMN, DROP and ADD PRIMARY KEY) run between the writes, and after every round the rows of the two must be
# the same through every plan, in a long snapshot and in a fresh one, and sift.check must find nothing. On Format 1 the VACUUMs queue behind a held maintenance lock while the writers
# close write buffers and the holder promotes. A seed that fails on the code before the fix is a probability, see the header of the script.
# One script for every job: SIFT_SEEDS (default 1-3; quick tier 2 of 8 s), SIFT_FUZZ_SECS (80; quick 8), SIFT_TEST_FORMAT (f2, f1 or release: the format of the library under test,
# default f2) and SIFT_FUZZ_DDL (1 or 0: DDL on or off, default on except on f1) select the run; the full tier runs the 24 seeds as eight jobs of 3 (SIFT_SEEDS=1-3, 4-6, ... 22-24).
# A seed takes about 125 s on a quiet machine and 200 s on a hosted runner (the 80 s of rounds, then the drain and the checks), so 24 seeds ran past the 1800 s of a hosted job.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
REPO=${SIFT_REPO:?}

SEEDS=$(knob SIFT_SEEDS 1-3 2)
SECS=$(knob SIFT_FUZZ_SECS 80 8)
HOLD=$(knob SIFT_FUZZ_HOLD 0.5 1)
FORMAT=${SIFT_TEST_FORMAT:-$([[ ${SIFT_FORMAT:-2} == 1 ]] && echo f1 || echo f2)}
DDL=()
case ${SIFT_FUZZ_DDL:-} in 1) DDL=(--ddl) ;; 0) DDL=(--no-ddl) ;; esac

node_init tw 36 "autovacuum = on
autovacuum_naptime = 2s
sift.maintenance_naptime = 200
sift.async_maintenance = on
sift.backend_cache_kb = ${SIFT_FUZZ_TOMB_LIMIT:-65536}
max_wal_size = 512MB
shared_buffers = 128MB
max_locks_per_transaction = 256"
node_start tw || bail "start failed"
sql tw "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
[[ $FORMAT == release ]] || sql tw "CREATE EXTENSION pg_sift_test" >/dev/null || bail "test extension failed"
plan 1

out=$(PGHOST=$(dir tw)/sock PGPORT=$(port tw) PGUSER=postgres SIFT_FUZZ_OUT=$(dir tw)/fuzz \
	python3 -I "$REPO/test/fuzz/tbl_twin.py" --format "$FORMAT" --seeds "$SEEDS" --seconds "$SECS" --hold "$HOLD" ${DDL[@]+"${DDL[@]}"} 2>&1)
rc=$?
for l in $(grep -o 'log=.*' <<<"$out" | cut -d= -f2); do
	grep -q 'FAIL\|HARNESS' <<<"$(grep "log=$l" <<<"$out")" && diag "$(tail -n 25 "$l" | cut -c1-300)"
done
check "the sift_table and its heap twin agree in every seed ($SEEDS, $FORMAT)" test "$rc" == 0
finish
