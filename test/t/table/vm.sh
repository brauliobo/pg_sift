#!/usr/bin/env bash
# The visibility map of a sift_table (table/f2/vm/vm.h): after VACUUM an index-only scan of a secondary index answers from the index without a fetch of the table (Heap Fetches: 0),
# chosen by the planner on its own; a delete or an update clears the bits first, so the same scan agrees with the table at every step; a prepared delete, a TRUNCATE, a crash
# after the clear and the reset of an unlogged table leave no bit that is not true. Needs a preloading server (the records of the map are those of a resource manager).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

ROWS=$(knob SIFT_VM_ROWS 60000 20000)
plan 25
node_init primary 0 "shared_preload_libraries = 'pg_sift'
wal_level = replica
max_prepared_transactions = 5
sift.test_create_format1 = off
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

PLAN="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)"
explain() { sql primary "SET max_parallel_workers_per_gather = 0" "$PLAN $1" | tail -n +1; }
fetches() { explain "$1" | sed -n 's/.*Heap Fetches: \([0-9][0-9]*\).*/\1/p'; }
via_index() { sql primary "SET max_parallel_workers_per_gather = 0" "SET enable_seqscan = off" "SET enable_bitmapscan = off" "$1"; }
via_table() { sql primary "SET enable_indexonlyscan = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "$1"; }
agree() { [[ $(via_index "$1" | tail -n 1) == "$(via_table "$1" | tail -n 1)" ]]; }
vm_bytes() { sql primary "SELECT pg_relation_size('$1', 'vm')"; }
COUNT="SELECT count(*), coalesce(sum(k), 0) FROM t WHERE k >= 0"

sql primary "CREATE TABLE t (id int, k int, v int, pad text) USING sift_table" \
	"INSERT INTO t SELECT g, g, 0, md5(g::text) FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX t_k ON t (k)" "ANALYZE t" >/dev/null || bail "setup failed"
SEL="SELECT k FROM t WHERE k BETWEEN 100 AND $((ROWS / 2))"

check "a table that was never vacuumed has no map and claims no all visible rows" test "$(sql primary "SELECT relallvisible FROM pg_class WHERE relname = 't'")" == 0
sql primary "VACUUM t" >/dev/null || bail "vacuum failed"
check "VACUUM stores the all visible blocks in pg_class" test "$(sql primary "SELECT relallvisible > 0 FROM pg_class WHERE relname = 't'")" == t
check "the map fork exists" test "$(vm_bytes t)" -gt 0
out=$(explain "$SEL")
check "the planner chooses an index-only scan on its own" grep -q "Index Only Scan using t_k" <<<"$out"
check "an index-only scan of the btree fetches no row of the table (Heap Fetches: 0)" test "$(fetches "$SEL")" == 0
check "the answer is the table's" agree "$COUNT"

# a delete clears the bits of the blocks it touches before it is logged
sql primary "DELETE FROM t WHERE id % 1000 = 0" >/dev/null || bail "delete failed"
check "after a DELETE the index-only scan agrees with the table" agree "$COUNT"
check "and it fetches the blocks of the deleted rows, not the whole table" test "$(fetches "$SEL")" -le "$((ROWS / 3))"
sql primary "UPDATE t SET v = v + 1 WHERE id % 777 = 0" >/dev/null || bail "update failed"
check "after an UPDATE of a column nothing indexes the scan agrees with the table" agree "$COUNT"
sql primary "UPDATE t SET k = k + 5000000 WHERE id % 500 = 1" >/dev/null || bail "key update failed"
check "after an UPDATE of the indexed column the scan agrees with the table" agree "$COUNT"
sql primary "INSERT INTO t SELECT g, g, 0, 'y' FROM generate_series($ROWS + 1, $ROWS + 3000) g" >/dev/null || bail "insert failed"
check "after an INSERT the scan agrees with the table" agree "$COUNT"
sql primary "VACUUM t" >/dev/null || bail "second vacuum failed"
check "after the second VACUUM the scan agrees with the table" agree "$COUNT"
check "and fetches almost nothing (95% skipped)" test "$(fetches "$SEL")" -le "$((ROWS / 40))"

# a prepared transaction holds the table: VACUUM sets nothing under it, and the delete it will commit is seen
sql primary "BEGIN" "DELETE FROM t WHERE id BETWEEN 10 AND 700" "PREPARE TRANSACTION 'vm1'" >/dev/null || bail "prepare failed"
sql primary "VACUUM t" >/dev/null || bail "vacuum under 2pc failed"
check "a prepared delete is not seen before it commits" agree "$COUNT"
sql primary "COMMIT PREPARED 'vm1'" >/dev/null || bail "commit prepared failed"
check "and is seen after" agree "$COUNT"

# the sessions of test/t/table/vm_iso/specs/basic.spec interleave INSERT, UPDATE, DELETE, a prepared DELETE, TRUNCATE and VACUUM with reads through the index and through the table
ISO=$(dirname "$(pg_config --pgxs)")/../test/isolation/pg_isolation_regress
isolation() {
	PGUSER=postgres "$ISO" --use-existing --dbname=postgres --host="$(dir primary)/sock" --port="$(port primary)" --inputdir="${SIFT_REPO:?}/test/t/table/vm_iso" --outputdir="$(dir primary)/iso" --bindir= tbl_vm \
		>"$(dir primary)/iso.log" 2>&1 || { out=$(cat "$(dir primary)/iso/results/tbl_vm.out.diff" 2>/dev/null); return 1; }
}
check "isolation: index-only reads agree with the table across concurrent INSERT, UPDATE, DELETE, 2PC, TRUNCATE and VACUUM" isolation

# crash after the clear: the bits the delete cleared stay cleared, the ones VACUUM set before it stay set
sql primary "VACUUM t" >/dev/null || bail "vacuum before the crash failed"
sql primary "DELETE FROM t WHERE id BETWEEN 20000 AND 20500" >/dev/null || bail "delete before the crash failed"
wait_for "wal_flushed primary" 30
crash primary
node_start primary || bail "restart after the crash failed"
check "crash recovery: the scan agrees with the table after the clear" agree "$COUNT"

# TRUNCATE takes the map with it
sql primary "VACUUM t" >/dev/null
check "VACUUM set the map again" test "$(vm_bytes t)" -gt 0
sql primary "TRUNCATE t" >/dev/null || bail "truncate failed"
check "TRUNCATE empties the map fork" test "$(vm_bytes t)" == 0
sql primary "INSERT INTO t SELECT g, g, 0, 'z' FROM generate_series(1, 5000) g" "VACUUM t" >/dev/null || bail "refill failed"
check "a refilled table is read right through the new map" agree "$COUNT"
check "and its index-only scan is answered by the map" test "$(fetches "SELECT k FROM t WHERE k BETWEEN 10 AND 4000")" == 0

# an unlogged table comes back empty after a crash, and so does its map
sql primary "CREATE UNLOGGED TABLE u (id int, k int) USING sift_table" "INSERT INTO u SELECT g, g FROM generate_series(1, 5000) g" "CREATE INDEX u_k ON u (k)" "VACUUM u" >/dev/null || bail "unlogged setup failed"
check "an unlogged table has a map" test "$(vm_bytes u)" -gt 0
crash primary
node_start primary || bail "restart after the crash failed"
check "the unlogged table is empty after the crash" test "$(sql primary "SELECT count(*) FROM u")" == 0
check "and its map is gone" test "$(vm_bytes u)" == 0
sql primary "INSERT INTO u SELECT g, g FROM generate_series(1, 2000) g" "ANALYZE u" >/dev/null || bail "insert after the reset failed"
check "rows inserted after the reset are read right through the index" agree "SELECT count(*), coalesce(sum(k), 0) FROM u WHERE k >= 0"
finish
