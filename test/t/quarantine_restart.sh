#!/usr/bin/env bash
# An autovacuum worker that meets the damage of a sift_col index finishes its VACUUM without the index and has the metapage flagged; after a restart the flag alone keeps the index out
# of service (sift.quarantine lists it, statements run on the heap), and REINDEX puts it back.
. "$(dirname "$0")/lib.bash"

plan 7
node_init p 72 "autovacuum = on
autovacuum_naptime = 1
sift.on_corruption = quarantine"
node_start p || bail "start failed"
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE a (id int, v int) WITH (autovacuum_vacuum_threshold = 10, autovacuum_vacuum_scale_factor = 0, autovacuum_analyze_threshold = 100000000)" \
	"INSERT INTO a SELECT i, i % 7 FROM generate_series(1, 20000) i" "CREATE INDEX a_idx ON a USING sift_col (id, v)" "ANALYZE a" >/dev/null || bail "setup failed"

uses_sift() { sql p "EXPLAIN (COSTS OFF) SELECT count(*) FROM a WHERE v = 3" | grep -qE 'Sift|a_idx'; }
state() { [[ $(sql p "SELECT persisted || ' ' || (skipped_vacuums > 0) FROM sift.quarantine WHERE index = 'a_idx'::regclass") == "$1" ]]; }
check "the index is planned before the damage" uses_sift

sql p "SELECT sift._test_poke('a_idx', 2, 8176, 9, 2)" "CHECKPOINT" "DELETE FROM a WHERE id <= 3000" >/dev/null || bail "damage failed"
check "autovacuum skips the damaged index, and the worker it asked for flags the metapage" wait_for "state 'true true'" 90
check "no autovacuum run failed with the damage" test "$(grep -c 'ERROR:  sift index' "$(dir p)/postgres.log")" == 0

node_stop p
node_start p || bail "restart failed"
want=$(sql p "SELECT count(*) FROM a WHERE v = 3")
check "after a restart the flag lists the index" test "$(sql p "SELECT index || ' ' || persisted FROM sift.quarantine")" == "a_idx true"
heap_plan() { ! uses_sift; }
check "after a restart the statements are planned on the heap" heap_plan
sql p "REINDEX INDEX a_idx" >/dev/null
check "REINDEX takes the index out of the list and the rows are the heap's" test "$(sql p "SELECT count(*) FROM sift.quarantine")" == 0 -a "$(sql p "SELECT count(*) FROM a WHERE v = 3")" == "$want"
sql p "ANALYZE a" >/dev/null
check "the rebuilt index is planned again" uses_sift
finish
