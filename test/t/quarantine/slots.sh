#!/usr/bin/env bash
# No background worker slot is free when an index is quarantined: the registry holds it out of service, statements and writes run on the heap, the metapage is not flagged (persisted = false, one
# LOG line says why); when a slot is free again the next plan that leaves the index out asks for the worker, the flag is written, and it keeps the index out after a restart.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 10
node_init qs 28 "autovacuum = off
max_worker_processes = 3
max_parallel_workers = 2
sift.on_corruption = quarantine"
node_start qs || bail "start failed"
D=$(dir qs)
sql qs "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE a (id int, v int)" "INSERT INTO a SELECT g, g % 7 FROM generate_series(1, 20000) g" \
	"CREATE INDEX a_idx ON a USING sift_col (id, v)" "ANALYZE a" \
	"CREATE TABLE s (id int, pad text) WITH (parallel_workers = 2, fillfactor = 10)" "ALTER TABLE s ALTER COLUMN pad SET STORAGE PLAIN" \
	"INSERT INTO s SELECT g, repeat('x', 700) FROM generate_series(1, 6) g" >/dev/null || bail "setup failed"

uses_sift() { sql qs "EXPLAIN (COSTS OFF) SELECT count(*) FROM a WHERE v = 3" | grep -qE 'Sift|a_idx'; }
persisted() { [[ $(sql qs "SELECT persisted FROM sift.quarantine WHERE index = 'a_idx'::regclass") == t ]]; }
SLEEPERS="SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'parallel worker'"

# two parallel workers sleep in the slots of the server that are left by its launcher: the workers of sift find none
sql qs "SET parallel_setup_cost = 0" "SET parallel_tuple_cost = 0" "SET min_parallel_table_scan_size = 0" "SET parallel_leader_participation = off" \
	"SET max_parallel_workers_per_gather = 2" "SELECT count(*) FROM s WHERE pg_sleep(600) IS NOT NULL" >/dev/null 2>&1 &
holder=$!
slept() { [[ $(sql qs "$SLEEPERS") -ge 2 ]]; }
check "two parallel workers hold their slots" wait_for slept 60
check "the index is planned before the damage" uses_sift

sql qs "SELECT sift._test_poke('a_idx', 2, 8176, 9, 2)" >/dev/null
out=$(sql qs "SELECT count(*) FROM a WHERE v = 3" 2>&1)
check "the statement that meets the damage fails once" grep -q 'is corrupted' <<<"$out"
want=$(sql qs "SET sift.enable_scan = off" "SET sift.enable_agg = off" "SET enable_indexscan = off" "SELECT count(*) FROM a WHERE v = 3")
check "the next statement is planned on the heap and gives the rows of the table" test "$(sql qs "SELECT count(*) FROM a WHERE v = 3")" == "$want"
check "the index is out of service and not flagged" test "$(sql qs "SELECT persisted FROM sift.quarantine WHERE index = 'a_idx'::regclass")" == f
check "no worker slot is said in one LOG line" wait_for "grep -q 'no background worker for quarantine of index \"public.a_idx\"' '$D/postgres.log'" 30
sql qs "INSERT INTO a VALUES (20001, 3)" "VACUUM a" >/dev/null 2>&1
check "INSERT and VACUUM go on without the index" test "$(sql qs "SELECT count(*) FROM a WHERE id = 20001")" == 1

sql qs "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE '%pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null
wait "$holder" 2>/dev/null
asked() { sql qs "SELECT count(*) FROM a WHERE v = 3" >/dev/null 2>&1; persisted; }
check "a free slot and the next plan flag the metapage" wait_for asked 120
node_stop qs
node_start qs || bail "restart failed"
check "after a restart the flag keeps the index out and the statements run on the heap" test "$(sql qs "SELECT index || ' ' || persisted FROM sift.quarantine")" == "a_idx true" -a "$(uses_sift && echo sift)" != sift
sql qs "REINDEX INDEX a_idx" "ANALYZE a" >/dev/null
check "REINDEX puts the index back" test "$(sql qs "SELECT count(*) FROM sift.quarantine")" == 0 -a "$(uses_sift && echo sift)" == sift
finish
