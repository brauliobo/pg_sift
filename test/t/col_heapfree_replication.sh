#!/usr/bin/env bash
# The aggregate that reads without the heap on a real physical standby that streams the churn of the primary, on that standby once it is promoted, and on the
# subscriber of a logical replication, which is no place for track_deletes (the apply worker fires no statement trigger and writes no replacement). On every node the answer of the
# heap-free read is the oracle's (no sift plan); on the primary and the standby it reads fewer heap pages than the read that checks every page, on the subscriber it does not use the log.
. "$(dirname "$0")/lib.bash"

plan 17
ROWS=$(knob SIFT_HF_ROWS 20000 12000)
CONF="wal_level = logical
max_wal_senders = 5
max_replication_slots = 5
autovacuum = off
synchronous_commit = on
max_logical_replication_workers = 4
max_worker_processes = 16"
node_init p 34 "$CONF"
node_start p || bail "start failed"
D=$(dir p)

DDL="CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)"
IDX="CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_updates = 'id', track_deletes = on)"
sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "$DDL" \
	"INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, $ROWS) i" "$IDX" "CREATE PUBLICATION pb FOR TABLE events" \
	"VACUUM (FREEZE, ANALYZE) events" >/dev/null || bail "setup failed"
node_clone p s 35 "max_worker_processes = 16"
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
standby_ok() { saves s && [[ $(ans s free) == "$(ans p oracle)" ]]; }
# the churn of one round: a HOT update, a covered update, a key change, a delete, new rows, an aborted change
churn() {
	local r=$1
	sql "${2:-p}" "UPDATE events SET label = 'r$r' WHERE id % 7 = $((r % 7))" "UPDATE events SET counter_a = counter_a + $r WHERE id % 11 = $((r % 11))" \
		"UPDATE events SET id = id + $((100000 * r)) WHERE id % 53 = $r" "DELETE FROM events WHERE id % 61 = $r" \
		"INSERT INTO events SELECT i, i % 10, i, 'n$r' FROM generate_series($((ROWS * 2 + r * 1000)), $((ROWS * 2 + r * 1000 + 300))) i" \
		"BEGIN; UPDATE events SET counter_a = 0 WHERE id % 67 = $r; DELETE FROM events WHERE id % 71 = $r; ROLLBACK" >/dev/null
}

plans_ok() { planned p && planned s; }
check "the aggregate is a sift plan on the primary and the standby" plans_ok
check "fresh: the primary reads no heap page, the standby none, and every answer is the oracle's" test "$(pages p free)" == 0 -a "$(pages s free)" == 0
check "fresh: primary and standby give one answer" test "$(ans p free)" == "$(ans s free)" -a "$(ans s free)" == "$(ans s oracle)"

# a backend of the standby that stays: it reads the log once and keeps what it cached while the primary churns, VACUUM gives addresses to new rows, and the WAL is replayed under it
cat >"$D/live.sql" <<EOF
\\set QUIET on
\\pset tuples_only on
\\pset format unaligned
SET max_parallel_workers_per_gather = 0;
\\! touch '$D/live.ready'
EOF
for n in 1 2 3; do
	cat >>"$D/live.sql" <<EOF
\\! while [ ! -e '$D/req$n' ]; do sleep 0.05; done
SET sift.enable_agg = on;
\\o $D/live$n.free
$A;
\\o
SET sift.enable_agg = off;
\\o $D/live$n.oracle
$A;
\\o
\\! touch '$D/done$n'
EOF
done
psql "$(conninfo s)" -X -q -f "$D/live.sql" >"$D/live.log" 2>&1 &
lp=$!
until [[ -e $D/live.ready ]]; do sleep 0.05; done
touch "$D/req1"; until [[ -e $D/done1 ]]; do sleep 0.05; done

for r in 1 2 3; do
	churn "$r"
	catchup p s || bail "catchup $r"
	check "round $r: the standby's heap-free read is the oracle's (and so are the checked read's)" agree s
	check "round $r: the standby reads fewer heap pages than the read that checks them all, and its answer is the primary's" standby_ok
	if [[ $r == 2 ]]; then
		sql p "VACUUM events" "INSERT INTO events SELECT i, i % 10, i, 'v' FROM generate_series($((ROWS * 5)), $((ROWS * 5 + 800))) i" >/dev/null
		catchup p s || bail "catchup vacuum"
	fi
	touch "$D/req$r"; until [[ -e $D/done$r ]]; do sleep 0.05; done
	[[ $r -gt 1 ]] && check "round $r: the long-lived standby backend gives the oracle's answer as well" test "$(tr -d ' ' <"$D/live$r.free")" == "$(tr -d ' ' <"$D/live$r.oracle")" -a -s "$D/live$r.free"
done
wait "$lp"

# the subscriber: the table and its index with the option on; the apply worker writes the rows without the statement triggers, so the log is not the truth there
node_init sub 36 "$CONF"
node_start sub || bail "subscriber start failed"
sql sub "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "$DDL" "$IDX" "CREATE SUBSCRIPTION sb CONNECTION '$(conninfo p)' PUBLICATION pb" >/dev/null || bail "subscription failed"
for _ in $(seq 100); do [[ $(sql sub "SELECT count(*) FROM events") == "$(sql p "SELECT count(*) FROM events")" ]] && break; sleep 0.2; done
churn 4
for _ in $(seq 100); do [[ $(sql sub "SELECT count(*) FROM events") == "$(sql p "SELECT count(*) FROM events")" ]] && break; sleep 0.2; done
sql sub "VACUUM events" >/dev/null
check "subscriber: the same rows as the publisher" test "$(ans sub oracle)" == "$(ans p oracle)"
check "subscriber: the heap-free read is the oracle's, and the log is not taken for the truth" agree sub
check "subscriber: it reads the heap as the checked read does" test "$(pages sub free)" -ge "$(pages sub checked)"

# the standby becomes the primary: its writes churn the index with the log and the replacement as any primary's
catchup p s || bail "catchup before promotion"
node_promote s
until [[ $(sql s "SELECT pg_is_in_recovery()") == f ]]; do sleep 0.2; done
sql s "VACUUM (FREEZE) events" >/dev/null
check "promoted: after VACUUM FREEZE, the heap-free read is the oracle's with no heap page" test "$(ans s free)" == "$(ans s oracle)" -a "$(pages s free)" == 0
churn 5 s
check "promoted: after churn on the new primary the read is the oracle's" agree s
check "promoted: and it reads fewer heap pages than the read that checks them" saves s

finish
