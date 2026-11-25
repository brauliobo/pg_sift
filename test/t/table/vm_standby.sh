#!/usr/bin/env bash
# The visibility map of a sift_table on a hot standby with hot_standby_feedback = off. The primary does not know the old snapshot of the standby, so its VACUUM finds the rows of an
# insert that the snapshot cannot see all visible and sets their bits. The record that sets them carries the newest transaction it makes visible, and the standby ends the queries whose
# snapshot is not past it before it applies the bits (ResolveRecoveryConflictWithSnapshot): the index-only scan of the old snapshot never skips the table for a row it must not see.
# An insert-only workload: no deletion vector is settled, so the only record of the VACUUM that conflicts with a reader is the one of the map.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

ROWS=$(knob SIFT_VM_ROWS 30000 10000)
plan 11
node_init primary 0 "shared_preload_libraries = 'pg_sift'
wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
sift.test_create_format1 = off
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, k int, pad text) USING sift_table" "INSERT INTO t SELECT g, g, md5(g::text) FROM generate_series(1, $ROWS) g" \
	"CREATE INDEX t_k ON t (k)" "ANALYZE t" "VACUUM t" >/dev/null || bail "setup failed"
node_clone primary standby 1 "hot_standby_feedback = off
wal_receiver_status_interval = 1s
max_standby_streaming_delay = 1s
log_recovery_conflict_waits = on
deadlock_timeout = 200ms"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"
D=$(dir standby)
COUNT="SELECT count(*) FROM t WHERE k >= 0"
FETCHES() { sql "$1" "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k FROM t WHERE k BETWEEN 100 AND $((ROWS / 2))" |
	sed -n 's/.*Heap Fetches: \([0-9][0-9]*\).*/\1/p'; }

check "the standby reads the bits the first VACUUM set: no fetch of the table" test "$(FETCHES standby)" == 0
check "the standby counts as the primary" test "$(sql standby "$COUNT")" == "$(sql primary "$COUNT")"

# An insert of the primary that is still running when the standby takes its snapshot: replayed (the standby has its rows and its index entries), invisible to the snapshot. The standby
# holds the snapshot until $D/release exists; the primary commits and VACUUMs, which finds the rows all visible, as the feedback that would hold it back is off.
rm -f "$D/locked" "$D/release" "$D/inserted" "$D/commit"
{
	echo "BEGIN;"
	echo "INSERT INTO t SELECT g, g, md5(g::text) FROM generate_series($ROWS + 1, $ROWS + 5000) g;"
	echo "\\! touch '$D/inserted'"
	echo "\\! while [ ! -e '$D/commit' ]; do sleep 0.05; done"
	echo "COMMIT;"
} | sql primary >"$D/insert.out" 2>&1 &
INSERTER=$!
until [[ -e $D/inserted ]]; do sleep 0.05; done
catchup primary standby || bail "the standby did not replay the insert"
{
	echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"
	echo "SELECT count(*) FROM t;"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
	echo "SET enable_seqscan = off; SET enable_bitmapscan = off;"
	echo "SELECT 'rows seen by the old snapshot: ' || count(*) FROM t WHERE k >= 0;"
	echo "COMMIT;"
} | sql standby >"$D/query.out" 2>&1 &
HOLDER=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
touch "$D/commit"
wait "$INSERTER"
before=$(sql standby "SELECT confl_snapshot FROM pg_stat_database_conflicts WHERE datname = 'postgres'")
sql primary "VACUUM (TRUNCATE false) t" >/dev/null || bail "vacuum failed"
check "the primary sets the bits of the new rows although the standby holds an old snapshot (its feedback is off)" test "$(sql primary "SELECT relallvisible > 0 FROM pg_class WHERE relname = 't'")" == t
catchup primary standby || bail "the standby did not replay the VACUUM"
wait_for "[[ \$(sql standby \"SELECT confl_snapshot FROM pg_stat_database_conflicts WHERE datname = 'postgres'\") -gt $before ]]" 30
check "the replay of the VACUUM ended the query of the old snapshot with a snapshot conflict" test "$(sql standby "SELECT confl_snapshot > $before FROM pg_stat_database_conflicts WHERE datname = 'postgres'")" == t
check "the record that waited for it is the one that sets the bits" grep -q "WAL redo at .* for pg_sift_table_vm/VM_SET" "$D/postgres.log"
touch "$D/release"
wait "$HOLDER"
check "the old snapshot was ended with a conflict with recovery" grep -qi "conflict with recovery" "$D/query.out"
check "and read no row of the insert it cannot see (no phantom)" test -z "$(grep 'rows seen by the old snapshot' "$D/query.out")"

check "a new snapshot of the standby counts what the primary counts" test "$(sql standby "$COUNT")" == "$(sql primary "$COUNT")"
check "and the index-only scan of the standby fetches no row of the table" test "$(FETCHES standby)" == 0
check "the standby verifies its map as the primary does" test "$(sql standby "SELECT relallvisible FROM pg_class WHERE relname = 't'")" == "$(sql primary "SELECT relallvisible FROM pg_class WHERE relname = 't'")"

# with the feedback on, the primary holds back: the rows the old snapshot cannot see are not all visible for the primary, and the snapshot is not cancelled
node_stop standby
node_conf standby "hot_standby_feedback = on"
node_start standby || bail "standby restart failed"
catchup primary standby
rm -f "$D/locked" "$D/release"
{
	echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"
	echo "SELECT count(*) FROM t;"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
	echo "SET enable_seqscan = off; SET enable_bitmapscan = off;"
	echo "SELECT 'rows seen by the old snapshot: ' || count(*) FROM t WHERE k >= 0;"
	echo "COMMIT;"
} | sql standby >"$D/query2.out" 2>&1 &
HOLDER=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sleep 3
sql primary "INSERT INTO t SELECT g, g, md5(g::text) FROM generate_series($ROWS + 5001, $ROWS + 8000) g" "VACUUM (TRUNCATE false) t" >/dev/null || bail "second insert failed"
catchup primary standby
touch "$D/release"
wait "$HOLDER"
check "feedback on: the old snapshot lives and reads the rows it saw, not the new ones" grep -q "rows seen by the old snapshot: $((ROWS + 5000))" "$D/query2.out"
finish
