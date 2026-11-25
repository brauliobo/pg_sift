#!/usr/bin/env bash
# sift.drop_parts_before and the visibility map of a sift_table: the parts that leave the table take their bits with them, so an index-only scan of a secondary index never answers a row of a
# dropped part; the function is not transactional and refuses a transaction block, so that a ROLLBACK cannot promise rows back that are gone. Needs a preloading server (the map is logged by a resource manager).
. "$(dirname "$0")/lib.bash"

DAYS=10
ROWS=5000
plan 6
node_init primary 0 "shared_preload_libraries = 'pg_sift'
wal_level = replica
sift.test_create_format1 = off
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

PLAN="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)"
SEL="SELECT id FROM t WHERE id >= 0"
via_index() { sql primary "SET max_parallel_workers_per_gather = 0" "SET enable_seqscan = off" "SET enable_bitmapscan = off" "$1"; }
via_table() { sql primary "SET enable_indexonlyscan = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "$1"; }
agree() { [[ $(via_index "$1" | tail -n 1) == "$(via_table "$1" | tail -n 1)" ]]; }
fetches() { sql primary "SET max_parallel_workers_per_gather = 0" "SET enable_seqscan = off" "SET enable_bitmapscan = off" "$PLAN $1" | sed -n 's/.*Heap Fetches: \([0-9][0-9]*\).*/\1/p'; }
COUNT="SELECT count(*) FROM (SELECT id FROM t WHERE id >= 0 OFFSET 0) s"

sql primary "CREATE TABLE t (id int, at timestamptz NOT NULL, pad text) USING sift_table WITH (autovacuum_enabled = false)" "CREATE INDEX t_id ON t (id)" >/dev/null || bail "setup failed"
for ((d = 0; d < DAYS; d++)); do
	sql primary "INSERT INTO t SELECT $d * $ROWS + g, timestamptz '2030-01-01' + $d * interval '1 day' + g * interval '1 second', md5(g::text) FROM generate_series(1, $ROWS) g" "VACUUM (FREEZE) t" >/dev/null || bail "load failed"
done
sql primary "VACUUM t" >/dev/null || bail "vacuum failed"

check "VACUUM set the map: the index-only scan fetches few rows of the table" test "$(fetches "$SEL")" -lt "$ROWS"
sql primary "SELECT parts_dropped FROM sift.drop_parts_before('t', timestamptz '2030-01-06')" >/dev/null || bail "drop failed"
check "the table lost the five days" test "$(sql primary "$COUNT")" == "$((5 * ROWS))"
check "the index-only scan agrees with the table after the drop" agree "$COUNT"
check "the bits of the dropped parts are gone: the scan visits the table for their index entries" test "$(fetches "$SEL")" -ge "$((5 * ROWS))"
check "a ROLLBACK cannot undo the drop: the function refuses a transaction block" test "$(sql primary "BEGIN" "SELECT parts_dropped FROM sift.drop_parts_before('t', timestamptz '2030-01-08')" "ROLLBACK" 2>&1 | grep -c 'cannot run inside a transaction block')" == 1
check "and nothing more was dropped" test "$(sql primary "$COUNT")" == "$((5 * ROWS))"
finish
