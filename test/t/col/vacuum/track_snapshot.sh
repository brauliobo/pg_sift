#!/usr/bin/env bash
# A lazy VACUUM runs with PROC_IN_VACUUM: every snapshot leaves its transaction id out. The VACUUM re-baselines the track_changes log of an index built concurrently (a tombstone for every
# address no version is live for, stamped with the transaction id the VACUUM took, and the log names that id as its base). A snapshot taken while the VACUUM runs finds that id below its
# xmax and not running, so after the VACUUM commits it counts the baseline as made before the snapshot. The baseline tombstones are only for addresses that no version is live for, and a
# row that a session deletes meanwhile leaves its page without the all-visible bit, which the heap-free read checks: the older snapshot must still read every row it saw.
# The VACUUM stops (sift.test_cleanup_gate, a testing build) right after it took the id; a REPEATABLE READ session takes its snapshot, a second session deletes rows, the VACUUM goes on and
# commits, and the older snapshot reads again, by the heap and by the heap-free scan.
. "$(dirname "$0")/lib.bash"

plan 4
node_init vt 49 "autovacuum = off
sift.async_maintenance = off
sift.log_min_duration = 0"
node_start vt || bail "start failed"
D=$(dir vt)
sql vt "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT i, i % 10, i FROM generate_series(1, 20000) i" "VACUUM ANALYZE t" \
	"CREATE INDEX CONCURRENTLY t_c ON t USING sift_col (id, tenant, amount) WITH (track_changes = id)" >/dev/null 2>&1 || bail "setup failed"

Q="SELECT string_agg(tenant || ':' || n || ':' || s, ' ' ORDER BY tenant) FROM (SELECT tenant, count(*) n, sum(amount) s FROM t GROUP BY tenant) q"
ans() { echo "SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = $1; $Q;"; }

rm -f "$D"/{locked,snapped,release,vacuumed,out}
{
	echo "SELECT pg_advisory_lock(20983, 11);"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
} | sql vt >/dev/null &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sql vt "LOAD 'pg_sift'" "SET sift.test_cleanup_gate = on" "VACUUM t" >"$D/vacuum" 2>&1 &
vp=$!
for _ in $(seq 400); do [[ $(sql vt "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break; sleep 0.05; done
{
	echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"
	echo "SET max_parallel_workers_per_gather = 0;"
	echo "SET sift.enable_agg = off;"
	echo "\\o '$D/before'"
	echo "$Q;"
	echo "\\! touch '$D/snapped'"
	echo "\\! while [ ! -e '$D/vacuumed' ]; do sleep 0.05; done"
	echo "\\o '$D/heap'"
	echo "$Q;"
	echo "SET sift.enable_agg = on;"
	echo "\\o '$D/free'"
	echo "$Q;"
	echo "COMMIT;"
} | sql vt >/dev/null 2>&1 &
sp=$!
until [[ -e $D/snapped ]]; do sleep 0.05; done
sql vt "DELETE FROM t WHERE id % 3 = 0" >/dev/null
touch "$D/release"
wait "$vp" "$hp"
touch "$D/vacuumed"
wait "$sp"

check "the VACUUM re-baselined the log: heap-free reads" test "$(sql vt "SELECT heap_free FROM sift.col_tracking('t_c')")" == t
check "the older snapshot reads the same rows from the heap after the VACUUM" test "$(cat "$D/heap")" == "$(cat "$D/before")"
check "... and the heap-free read of the older snapshot is the heap's answer" test "$(cat "$D/free")" == "$(cat "$D/heap")"
check "the log has no error" test "$(grep -cE 'ERROR|TRAP' "$D/postgres.log")" == 0
finish
