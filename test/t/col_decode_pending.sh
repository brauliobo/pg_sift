#!/usr/bin/env bash
# A tool that applies changes while it decodes (pg_squeeze does) runs the replay of a decoded transaction as a subtransaction under a historic snapshot, which reads the catalogs as of the past. The
# rows that the transaction had buffered for a sift_col index (the rows after its first) must reach the index when that subtransaction starts: they were taken for the rows of an index that
# is gone, because the index (a copy that is made in the transaction) is not in the old catalogs, and cleared. Here a transaction that inserts into a table and decodes a slot in the same
# transaction, with the index made in the transaction as well as an earlier one.
. "$(dirname "$0")/lib.bash"

plan 6
node_init d 40 "wal_level = logical
max_replication_slots = 4
max_wal_senders = 4
autovacuum = off"
node_start d || bail "start failed"
export PGOPTIONS="-c client_min_messages=warning"

# test_decoding is a contrib module that some builds (a PostgreSQL built from git without contrib) lack: a throwaway slot tells
sql d "SELECT pg_create_logical_replication_slot('probe', 'test_decoding')" >/dev/null 2>&1 || { skip 6 "the test_decoding module is not installed"; finish; }
sql d "SELECT pg_drop_replication_slot('probe')" >/dev/null || bail "the probe slot was not dropped"

same() { # the groups of the table by the index and by the heap
	sql d "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "SET seq_page_cost = 10000" \
		"SELECT md5(string_agg(g || ':' || c || ':' || s, ',' ORDER BY g)) FROM (SELECT g, count(*) c, sum(v) s FROM $1 GROUP BY g) x" | tail -n 1
}
heap() {
	sql d "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = off" \
		"SELECT md5(string_agg(g || ':' || c || ':' || s, ',' ORDER BY g)) FROM (SELECT g, count(*) c, sum(v) s FROM $1 GROUP BY g) x" | tail -n 1
}

sql d "CREATE EXTENSION pg_sift" \
	"CREATE TABLE ev (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false)" "CREATE TABLE other (x int)" \
	"INSERT INTO ev SELECT i, i % 7, i FROM generate_series(1, 2000) i" \
	"CREATE INDEX ev_c ON ev USING sift_col (id, g, v) WITH (track_changes = 'off')" \
	"SELECT pg_create_logical_replication_slot('s', 'test_decoding')" "INSERT INTO other VALUES (1)" >/dev/null || bail "setup failed"

# the index of the table: its rows buffered while the transaction decodes
sql d "BEGIN" "INSERT INTO ev SELECT i, i % 7, i FROM generate_series(10001, 10050) i" "INSERT INTO ev SELECT i, i % 7, i FROM generate_series(10051, 10100) i" \
	"SELECT count(*) FROM pg_logical_slot_get_changes('s', NULL, NULL)" "COMMIT" >/dev/null
check "rows buffered when a transaction decodes are in the index" test "$(sql d "SELECT sift.col_verify('ev_c', true)" 2>&1)" == ""
check "and a group read by the index answers as the heap" test "$(same ev)" == "$(heap ev)"

# an index made in the transaction, as the copy of pg_squeeze is, and the rows of the transaction after it
sql d "INSERT INTO other VALUES (2)" >/dev/null
sql d "BEGIN" "CREATE TABLE cp (LIKE ev)" "INSERT INTO cp SELECT * FROM ev" "CREATE INDEX cp_c ON cp USING sift_col (id, g, v) WITH (track_changes = 'off')" \
	"INSERT INTO cp SELECT i, i % 7, i FROM generate_series(20001, 20050) i" "INSERT INTO cp SELECT i, i % 7, i FROM generate_series(20051, 20100) i" \
	"SELECT count(*) FROM pg_logical_slot_get_changes('s', NULL, NULL)" "COMMIT" >/dev/null
check "an index made in the transaction holds the rows buffered while it decodes" test "$(sql d "SELECT sift.col_verify('cp_c', true)" 2>&1)" == ""
check "and answers as the heap" test "$(same cp)" == "$(heap cp)"
check "the table has all the rows" test "$(sql d "SELECT count(*) FROM ev")" == 2100 -a "$(sql d "SELECT count(*) FROM cp")" == 2200
check "the server did not fail" test "$(sql d "SELECT 1")" == 1
finish
