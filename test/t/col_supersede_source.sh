#!/usr/bin/env bash
# The entries a new row version replaces get a tombstone when the UPDATE writes it, and the addresses of the key come from the table's unique index. What the single-session test
# (col_supersede_source) cannot show: a row that two sessions update, one waiting for the other (the waiting statement updates the version the first wrote, whose entry the index got
# while the first still ran), a key that one session gives up and another takes, a delete and the key coming back, and an update that is rolled back under the waiting one. After each
# the heap-free read, which trusts the log, gives the same answer as no sift plan; a mutant that finds no address (sift.test_no_supersede with the trigger off) does not.
. "$(dirname "$0")/lib.bash"

plan 9
ROWS=$(knob SIFT_SS_ROWS 20000 12000)
node_init p 33 "autovacuum = off"
node_start p || bail "start failed"
D=$(dir p)

sql p "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)" \
	"INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, $ROWS) i" \
	"CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id')" \
	"VACUUM (FREEZE, ANALYZE) events" >/dev/null || bail "setup failed"

Q="SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM events GROUP BY tenant_id"
A="SELECT md5(coalesce(string_agg(tenant_id || ':' || n || ':' || s, ',' ORDER BY tenant_id), 'none')) FROM ($Q) q"
MODE_free="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = on; SET sift.test_no_heapfree = off"
MODE_oracle="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = off; SET sift.test_no_heapfree = off"
ans() { local m=MODE_$1; sql p "${!m}" "$A"; }
plan_free() { sql p "${MODE_free}" "EXPLAIN (COSTS OFF) $Q" | grep -q SiftAgg; }
agree() { local f=$(ans free) o=$(ans oracle); [[ $f == "$o" ]] || { diag "free $f oracle $o"; return 1; }; }
check "the aggregate is a sift plan" plan_free

# two sessions: the first holds its change until the release file appears, the second runs a statement that waits for it
two() {
	local first=$1 second=$2 rel=${3:-COMMIT} n
	n=$((++SCEN))
	rm -f "$D/locked$n" "$D/release$n"
	{ echo "BEGIN;"; echo "$first;"; echo "\\! touch '$D/locked$n'"; echo "\\! while [ ! -e '$D/release$n' ]; do sleep 0.05; done"; echo "$rel;"; } | sql p >"$D/s1.$n" 2>&1 &
	local p1=$!
	until [[ -e $D/locked$n ]]; do sleep 0.05; done
	sql p "$second" >"$D/s2.$n" 2>&1 &
	local p2=$!
	sleep 0.5
	touch "$D/release$n"
	wait "$p1" "$p2"
}
SCEN=0

# 1: the second session updates the row the first changed: the version it updates is the first's, whose entry is in the index
two "UPDATE events SET counter_a = counter_a + 1 WHERE id % 7 = 0" "UPDATE events SET counter_a = counter_a + 10 WHERE id % 7 = 0"
check "two sessions update the same rows: the heap-free read is the oracle's" agree
# 2: the first moves the keys away, the second takes the keys the first leaves
two "UPDATE events SET id = id + 1000000 WHERE id % 11 = 0" "UPDATE events SET counter_a = counter_a + 5 WHERE id % 11 = 0"
check "the first moves the keys, the second updates the rows by them: the oracle's answer" agree
# 3: the first deletes, the second inserts the keys again once the delete ends
two "DELETE FROM events WHERE id % 13 = 0" "INSERT INTO events SELECT i, i % 10, i, 'again' FROM generate_series(13, $ROWS, 13) i ON CONFLICT DO NOTHING"
check "a delete and the keys coming back: the oracle's answer" agree
# 4: the first rolls back under the waiting second
two "UPDATE events SET counter_a = counter_a + 1 WHERE id % 17 = 0" "UPDATE events SET counter_a = counter_a + 20 WHERE id % 17 = 0" ROLLBACK
check "the first session rolls back: the oracle's answer" agree
# 5: the first updates through an upsert, the second updates the same rows
two "INSERT INTO events SELECT i, i % 10, i, 'u' FROM generate_series(19, $ROWS, 19) i ON CONFLICT (id) DO UPDATE SET counter_a = events.counter_a + 3" "UPDATE events SET counter_a = counter_a + 30 WHERE id % 19 = 0"
check "an upsert under an update: the oracle's answer" agree
# 6: the second one is an update of the key
two "UPDATE events SET counter_a = counter_a + 1 WHERE id % 23 = 0" "UPDATE events SET id = id + 2000000 WHERE id % 23 = 0"
check "an update of the rows and then of their keys: the oracle's answer" agree
check "no session failed" test "$(cat "$D"/s1.* "$D"/s2.* | grep -c ERROR)" == 0

# the mutant that finds no address leaves the replaced entries live, and the log-trusting read counts them
mutant_wrong() { sql p "SET sift.test_no_supersede = on; SET sift.test_no_track = on; UPDATE events SET counter_a = counter_a + 1 WHERE id % 29 = 0" >/dev/null; [[ $(ans free) != "$(ans oracle)" ]]; }
check "the mutant (no tombstone for the replaced entries) is wrong" mutant_wrong

finish
