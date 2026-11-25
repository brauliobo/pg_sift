#!/usr/bin/env bash
# The directory of the shared tables (col/dsmreg.c) is a segment of the registry of PostgreSQL that the first backend that asks for any table makes. PostgreSQL raises a shortage there (no room
# in /dev/shm, no free segment) as an ordinary error, where no subtransaction can catch it (a parallel operation, a statement that writes, a background worker), and keeps an entry for the name that
# the failed creation never filled. The room is made sure of first, in sift_dsm_try's own way, so that a shortage is "no table" for every caller: each goes without its table, with one line in the
# log, and asks again later. sift.test_dsm_fail (in the configuration here, so that no session of the test makes the directory) fails the segment of the directory as a full /dev/shm does.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
plan 9

node_init dq 54 "autovacuum = off
sift.test_dsm_fail = 'pg_sift_dsm_dir'"
node_start dq || bail "start failed"
D=$(dir dq)
sql dq "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE w (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX w_c ON w USING sift_col (id, g, v) WITH (track_changes = 'id')" "INSERT INTO w SELECT i, i % 7, i FROM generate_series(1, 20) i" >/dev/null || bail "setup failed"
logged() { grep -c "$1" "$D/postgres.log" || true; }
q() { PGOPTIONS="-c client_min_messages=warning" sql dq "LOAD 'pg_sift'" "$@" 2>"$D/err"; }

# the callers that cannot start a subtransaction, and the one that can
L0=$(logged "are not shared with other backends")
check "a lookup table in a parallel operation: no table, no error, one line in the log" test "$(q "SELECT sift._test_dsm_caller('lookup_rows', true)")|$(cat "$D/err")|$(($(logged "are not shared with other backends") - L0))" == "f||1"
check "the dictionaries in a parallel worker: no table, no error" test "$(q "SELECT sift._test_dsm_caller('dictionary_worker', true)")|$(cat "$D/err")" == "f|"
check "a cache outside a parallel operation goes without its table, with no error" test "$(q "SELECT sift._test_dsm_attach('dq_probe', 1024)")|$(cat "$D/err")" == "f|"
L0=$(logged "references of the write buffers are not shared")
q "INSERT INTO w SELECT i, i % 7, i FROM generate_series(21, 3000) i" >/dev/null
check "a statement that writes finds no arena: it ends with no error, its rows carry their values, one line in the log" \
	test "$(cat "$D/err")|$(sql dq "SELECT count(*) FROM w")|$(($(logged "references of the write buffers are not shared") - L0))" == "|3000|1"
agg() { sql dq "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(g || ':' || n || ':' || s, ' ' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM w GROUP BY g) q" | tail -1; }
check "the aggregate through the index is the heap's" test "$(agg on)" == "$(agg off)"
L0=$(logged "the rollup refresh worker has no queue to serve")
check "the rollup refresh worker is started" test "$(sql dq "SELECT sift._test_col_rollup_worker()")" == t
wait_for '[[ $(logged "the rollup refresh worker has no queue to serve") -gt '"$L0"' ]]' 60
check "... it says it has no queue to serve and ends" test "$(($(logged "the rollup refresh worker has no queue to serve") - L0))" == 1
wait_for '[[ $(sql dq "SELECT count(*) FROM pg_stat_activity WHERE backend_type = '"'"'sift rollup refresh'"'"'") == 0 ]]' 30
check "no statement and no worker ended with an error" test "$(grep -c 'ERROR:' "$D/postgres.log" || true)|$(sql dq "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift rollup refresh'")" == "0|0"

# the room comes back: the directory is made by the next backend that asks, and the registry keeps nothing of the failed attempts
sed_i "/^sift.test_dsm_fail/d" "$D/data/postgresql.conf"
sql dq "SELECT pg_reload_conf()" >/dev/null
check "with room again, the next ask makes the directory and the table" wait_for '[[ $(sql dq "SELECT sift._test_dsm_attach('"'"'dq_probe'"'"', 1024)") == t ]]' 30
finish
