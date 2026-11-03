#!/usr/bin/env bash
# A tombstone, and the transaction that made the log of track_changes complete, name transactions; the commit log is truncated up to what the tables are frozen up to, and neither is
# counted in that. With the real truncation (about two million transactions, VACUUM FREEZE of every database): a read does not ask the log about a transaction it no longer keeps (no error),
# it goes to the heap for the pages the tombstone is on, or for all of them when the transaction that made the log complete is gone, and every answer is the oracle's.
. "$(dirname "$0")/lib.bash"

plan 14
BURN=$(knob SIFT_HF_BURN 2200000 2200000)
node_init p 31 "autovacuum = off
max_wal_size = 512MB"
node_start p || bail "start failed"

Q="SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM TBL GROUP BY tenant_id"
mode_free="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = off"
mode_checked="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = on"
mode_oracle="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = off; SET sift.test_no_heapfree = off"
ans() { local m=mode_$2 q=${Q//TBL/$3}; sql "$1" "${!m}" "SELECT md5(coalesce(string_agg(tenant_id || ':' || n || ':' || s, ',' ORDER BY tenant_id), 'none')) FROM ($q) q"; }
pages() { local m=mode_$2 q=${Q//TBL/$3}; sql "$1" "${!m}" "EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) $q" | grep '"Heap Pages"' | tr -dc 0-9; }
NODE=p
agree() { local f=$(ans $NODE free "$1") c=$(ans $NODE checked "$1") o=$(ans $NODE oracle "$1"); [[ $f == "$c" && $f == "$o" ]] || { diag "$1: free $f checked $c oracle $o"; return 1; }; }
tracking() { sql p "SELECT heap_free FROM sift.col_tracking('$1')"; }
entries() { sql p "SELECT count(*) FROM sift._test_col_log_entries('$1')"; }
frozen() { sql p "SELECT count(*) FROM sift._test_col_log_entries('$1') WHERE xid::text::bigint = 2"; }

sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extensions"
# ev_a: its index is vacuumed once after the build (the transaction that made the log complete is written as a frozen one); ev_b: never (it names a transaction that will be gone)
for t in ev_a ev_b; do
	sql p "CREATE TABLE $t (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)" \
		"INSERT INTO $t SELECT i, i % 10, i, 'x' FROM generate_series(1, 20000) i" \
		"CREATE INDEX ${t}_c ON $t USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id')" >/dev/null || bail "setup $t"
done
sql p "VACUUM (FREEZE, ANALYZE) ev_a" "VACUUM (FREEZE, INDEX_CLEANUP off, ANALYZE) ev_b" >/dev/null
# two rows are deleted, two deletes are rolled back: few dead rows, so a VACUUM leaves the entries and their tombstones
for t in ev_a ev_b; do
	sql p "DELETE FROM $t WHERE id IN (3, 4)" "BEGIN; DELETE FROM $t WHERE id IN (5, 6); ROLLBACK" >/dev/null
done
X0=$(sql p "SELECT min(xid::text::bigint) FROM sift._test_col_log_entries('ev_a_c') WHERE xid::text::bigint > 2")
check "before: the log holds the tombstones, every answer is the oracle's, and the heap is not read" test "$(entries ev_a_c)" -ge 2 -a "$(tracking ev_a_c)" == t -a "$(pages p free ev_a)" == 0
check "before: ev_a agrees" agree ev_a

# the transactions: a subtransaction takes a transaction number of its own
burn() { for _ in $(seq $((BURN / 200000))); do sql p "DO \$\$ BEGIN FOR i IN 1..200000 LOOP BEGIN INSERT INTO burn VALUES (i); EXCEPTION WHEN OTHERS THEN NULL; END; END LOOP; END \$\$" >/dev/null || return 1; done; }
sql p "CREATE UNLOGGED TABLE burn (i int)" >/dev/null
burn || bail "burning transactions"
# every database is frozen up to now and the indexes are left alone (INDEX_CLEANUP off): the commit log is truncated, the tombstones are as they were
sql p "DROP TABLE burn" >/dev/null
sql p "ALTER DATABASE template0 ALLOW_CONNECTIONS true" >/dev/null
for db in template0 template1 postgres; do
	psql "$(conninfo p) dbname=$db" -X -q -c "VACUUM (FREEZE, INDEX_CLEANUP off)" >/dev/null 2>&1 || bail "vacuum $db"
done
sql p "ALTER DATABASE template0 ALLOW_CONNECTIONS false" >/dev/null
gone() { [[ -z $(sql p "SELECT pg_xact_status('$X0'::xid8)") ]]; }
check "the commit log no longer knows the transaction of the first tombstone" gone

# a change of a column the index does not hold clears the visibility map of the pages and leaves the entries true: the heap is what checks them, or the log
sql p "UPDATE ev_a SET label = 'y' WHERE id % 7 = 0" "UPDATE ev_b SET label = 'y' WHERE id % 7 = 0" >/dev/null
check "ev_a (the log complete since a frozen transaction): the answers are the oracle's, with no error" agree ev_a
check "ev_a: the tombstone of the transaction beyond the log sends its page to the heap, the others are not read" test "$(pages p free ev_a)" -gt 0 -a "$(pages p free ev_a)" -lt "$(pages p checked ev_a)"
check "ev_b (the log complete by a transaction beyond the log): the answers are the oracle's, with no error" agree ev_b
check "ev_b: the transaction that made the log complete is beyond the log, so every page is checked in the heap" test "$(pages p free ev_b)" -ge "$(pages p checked ev_b)"
# the hot standby replays the log, the heap and the commit log truncation, and answers the same
node_clone p s 32 "" || bail "clone"
node_start s || bail "standby start"
catchup p s || bail "catchup"
NODE=s
check "standby: ev_a, the oracle's answers with no error" agree ev_a
check "standby: ev_b, the oracle's answers with no error" agree ev_b
check "standby: ev_a reads fewer heap pages than the checked read" test "$(pages s free ev_a)" -lt "$(pages s checked ev_a)"
NODE=p
# a VACUUM that skips the indexes sees the same transactions: the tombstones stay (nothing can be said of them) and none is frozen
sql p "VACUUM ev_a" >/dev/null
check "VACUUM does not ask the commit log about the tombstones that are beyond it, and keeps them" test "$(entries ev_a_c)" -ge 2 -a "$(frozen ev_a_c)" == 0
check "ev_a after that VACUUM: the oracle's answers" agree ev_a
# the log made complete again: the option off and on writes a baseline of the present
sql p "ALTER INDEX ev_b_c SET (track_changes = 'off')" "ALTER INDEX ev_b_c SET (track_changes = 'id')" >/dev/null
check "ev_b rebuilt log: complete again, a hundredth of the heap pages the checked read needs, and the answers agree" test "$(tracking ev_b_c)" == t -a "$(( $(pages p free ev_b) * 50 ))" -lt "$(pages p checked ev_b)" -a "$(ans p free ev_b)" == "$(ans p oracle ev_b)"
sql p "REINDEX INDEX ev_a_c" >/dev/null
check "ev_a REINDEX: no tombstone, no heap page, the oracle's answers" test "$(entries ev_a_c)" == 0 -a "$(pages p free ev_a)" == 0 -a "$(ans p free ev_a)" == "$(ans p oracle ev_a)"

finish
