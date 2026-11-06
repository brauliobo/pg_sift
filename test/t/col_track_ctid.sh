#!/usr/bin/env bash
# track_changes = 'ctid' (no key column: a table without a primary key, an index that does not hold the unique column) on what a regression test cannot show: a physical standby that
# streams the churn and its promotion, two sessions that update and delete the same row (the second waits, and the row is checked again against the version the first wrote:
# EvalPlanQual), and a crash of the primary (kill -9, the WAL replayed) with a transaction in flight. On every node the answer of the heap-free read is the oracle's (no sift plan);
# on the nodes whose log is complete it reads fewer heap pages than the read that checks every page. A session that writes no tombstone (sift.test_no_track) leaves a read that
# gives other rows, which is what makes the checks mean something.
. "$(dirname "$0")/lib.bash"

plan 20
ROWS=$(knob SIFT_HF_ROWS 20000 12000)
CONF="wal_level = replica
max_wal_senders = 5
max_replication_slots = 5
autovacuum = off
synchronous_commit = on
max_worker_processes = 16"
node_init p 44 "$CONF"
node_start p || bail "start failed"
D=$(dir p)

sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE events (id int NOT NULL, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 90)" \
	"CREATE UNIQUE INDEX events_id ON events (id)" \
	"INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX events_c ON events USING sift_col (tenant_id, counter_a) WITH (track_changes = 'ctid')" \
	"VACUUM (FREEZE, ANALYZE) events" >/dev/null || bail "setup failed"
node_clone p s 45 "max_worker_processes = 16"
node_start s || bail "standby start failed"
catchup p s || bail "standby catchup failed"

Q="SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM events GROUP BY tenant_id"
A="SELECT md5(coalesce(string_agg(tenant_id || ':' || n || ':' || s, ',' ORDER BY tenant_id), 'none')) FROM ($Q) q"
MODE_free="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = off"
MODE_checked="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = on"
MODE_oracle="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = off; SET sift.test_no_heapfree = off"
ans() { local m=MODE_$2; sql "$1" "${!m}" "$A"; }
pages() { local m=MODE_$2; sql "$1" "${!m}" "EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) $Q" | grep '"Heap Pages"' | tr -dc 0-9; }
planned() { sql "$1" "${MODE_free}" "EXPLAIN (COSTS OFF) $Q" | grep -q SiftAgg; }
agree() { local f=$(ans "$1" free) c=$(ans "$1" checked) o=$(ans "$1" oracle); [[ $f == "$c" && $f == "$o" ]] || { diag "$1: free $f checked $c oracle $o"; return 1; }; }
saves() { [[ $(pages "$1" free) -lt $(pages "$1" checked) ]]; }
sound() { agree "$1" && saves "$1"; }
standby_ok() { sound s && [[ $(ans s free) == "$(ans p oracle)" ]]; }
log_ok() { [[ $(sql p "SELECT complete AND heap_free FROM sift.col_tracking('events_c')") == t ]] && sound p; }
# the churn of one round: a HOT update twice (a chain), a covered update, a change of the unique column, a delete, new rows, an aborted change
churn() {
	local r=$1
	sql "${2:-p}" "UPDATE events SET label = 'r$r' WHERE id % 7 = $((r % 7))" "UPDATE events SET label = 's$r' WHERE id % 7 = $((r % 7))" \
		"UPDATE events SET counter_a = counter_a + $r WHERE id % 11 = $((r % 11))" \
		"UPDATE events SET id = id + $((100000 * r)) WHERE id % 53 = $r" "DELETE FROM events WHERE id % 61 = $r" \
		"INSERT INTO events SELECT i, i % 10, i, 'n$r' FROM generate_series($((ROWS * 2 + r * 1000)), $((ROWS * 2 + r * 1000 + 300))) i" \
		"BEGIN; UPDATE events SET counter_a = 0 WHERE id % 67 = $r; DELETE FROM events WHERE id % 71 = $r; ROLLBACK" >/dev/null
}

planned_both() { planned p && planned s; }
check "the aggregate is a sift plan on the primary and the standby" planned_both
check "the index tracks by address, its log is complete and a read may use it" test "$(sql p "SELECT track_changes AND triggers AND complete AND heap_free FROM sift.col_tracking('events_c')")" == t
check "fresh: the primary and the standby read no heap page" test "$(pages p free)" == 0 -a "$(pages s free)" == 0

for r in 1 2 3; do
	churn "$r"
	catchup p s || bail "catchup $r"
	check "round $r: the primary's heap-free read is the oracle's and reads fewer heap pages than the checked read" sound p
	check "round $r: so is the standby's, and its answer is the primary's" standby_ok
done

# a session that writes no tombstone: the read gives other rows, and a rebuild repairs them
sql p "SET sift.test_no_track = on" "DELETE FROM events WHERE id % 97 = 5" "UPDATE events SET counter_a = counter_a + 9 WHERE id % 89 = 3" >/dev/null
differs() { [[ $(ans p free) != "$(ans p oracle)" ]]; }
check "mutant: the deletes and updates of a session that wrote no tombstone leave the heap-free read other rows than the oracle" differs
sql p "REINDEX INDEX events_c" >/dev/null
check "after REINDEX the read is the oracle's again" sound p

# two sessions that change one row: the second waits for the first and works on the version it wrote
for i in 1 2 3 4; do
	(psql -X -At -q -h "$D/sock" -p "$(port p)" -U postgres -d postgres -c "BEGIN" -c "UPDATE events SET counter_a = counter_a + 100 WHERE id BETWEEN $((i * 100)) AND $((i * 100 + 20))" -c "SELECT pg_sleep(1.5)" -c "COMMIT" >/dev/null 2>&1 &)
	sleep 0.4
	if (( i % 2 )); then
		sql p "UPDATE events SET counter_a = counter_a + 1, label = 'q' WHERE id BETWEEN $((i * 100)) AND $((i * 100 + 20))" >/dev/null
	else
		sql p "DELETE FROM events WHERE id BETWEEN $((i * 100)) AND $((i * 100 + 20))" >/dev/null
	fi
done
check "a session that waited for another's update and then updated or deleted that version: the read is the oracle's" sound p

# a crash with a transaction in flight, the WAL replayed
(psql -X -At -q -h "$D/sock" -p "$(port p)" -U postgres -d postgres -c "BEGIN" -c "UPDATE events SET counter_a = counter_a + 7 WHERE id % 5 = 0" -c "DELETE FROM events WHERE id % 13 = 0" -c "SELECT pg_sleep(60)" >/dev/null 2>&1 &)
sleep 1
sql p "UPDATE events SET counter_a = counter_a + 3 WHERE id % 9 = 0" "DELETE FROM events WHERE id % 17 = 0" "UPDATE events SET label = 'z' WHERE id % 19 = 0" >/dev/null
before=$(ans p oracle)
crash p
node_start p || bail "restart after the crash failed"
check "after the crash the committed changes are there and those of the transaction in flight are not" test "$(ans p oracle)" == "$before"
check "after the crash the log is complete and the heap-free read is the oracle's" log_ok
sql p "UPDATE events SET counter_a = counter_a + 1 WHERE id % 23 = 0" "DELETE FROM events WHERE id % 29 = 0" >/dev/null
check "after the crash and more churn the read is the oracle's" sound p

# VACUUM gives the addresses of the deleted rows to new rows
sql p "VACUUM events" "INSERT INTO events SELECT i, i % 10, i, 'v' FROM generate_series($((ROWS * 50)), $((ROWS * 50 + 800))) i" >/dev/null
check "after VACUUM and new rows at the old addresses the read is the oracle's" agree p
sql p "UPDATE events SET label = 'w' WHERE id BETWEEN $((ROWS * 50)) AND $((ROWS * 50 + 800))" "DELETE FROM events WHERE id BETWEEN $((ROWS * 50)) AND $((ROWS * 50 + 300))" >/dev/null
check "the new rows deleted: the read is the oracle's" agree p
sql p "VACUUM (FREEZE) events" >/dev/null
check "after VACUUM FREEZE the read is the oracle's and reads no heap page" test "$(ans p free)" == "$(ans p oracle)" -a "$(pages p free)" == 0

# the standby becomes the primary
catchup p s || bail "catchup before promotion"
node_promote s
until [[ $(sql s "SELECT pg_is_in_recovery()") == f ]]; do sleep 0.2; done
check "promoted: the read is the oracle's" agree s
churn 5 s
check "promoted: after churn on the new primary the read is the oracle's and reads fewer heap pages than the checked read" sound s

finish
