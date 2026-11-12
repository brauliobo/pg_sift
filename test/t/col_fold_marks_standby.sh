#!/usr/bin/env bash
# The fold of the marks of the tombstone log into the liveness runs (sift._test_col_fold_marks, which the maintenance worker runs when the reads paid for it) clears rows a hot standby
# may still read through an older snapshot. It tells the standby first, as the prune record of a VACUUM does: without feedback the standby session that holds a snapshot older than the
# deletes is cancelled by the conflict (and the answers of the standby are the heap's afterwards); with feedback the primary sees the snapshot as the oldest one and folds nothing
# until it ends.
. "$(dirname "$0")/lib.bash"

plan 7
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE fm (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = off)" \
	"INSERT INTO fm SELECT i, i % 10, i FROM generate_series(1, 20000) i" \
	"CREATE INDEX fm_c ON fm USING sift_col (id, g, v) WITH (track_changes = 'id')" \
	"VACUUM (FREEZE, ANALYZE) fm" "CHECKPOINT" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = off
max_standby_streaming_delay = 1s
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

Q="SELECT count(*) || '/' || sum(v) FROM fm"
rows() { sql "$1" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $2" "$Q"; }
agree() { test "$(rows "$1" on)" == "$(rows "$1" off)"; }
fold() { sql primary "SELECT sift._test_col_fold_marks('fm_c')"; }
# a REPEATABLE READ session of the standby that holds its snapshot for a minute: its output is in $1 (ends by itself or when the conflict cancels it)
reader() {
	sql standby "SET application_name = 'fm_reader'" "BEGIN ISOLATION LEVEL REPEATABLE READ" "SET sift.enable_agg = off" "$Q" "SELECT pg_sleep(60)" "$Q" >"$1" 2>&1 &
	wait_for "[[ \$(sql standby \"SELECT count(*) FROM pg_stat_activity WHERE application_name = 'fm_reader' AND query LIKE '%pg_sleep%'\") == 1 ]]" 60
}
stop_reader() { sql standby "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'fm_reader'" >/dev/null; wait; }

# without feedback: the snapshot of the standby is older than the deletes; the conflict cancels it
reader "$(dir standby)/r1.out"
sql primary "DELETE FROM fm WHERE id % 100 = 7" >/dev/null || bail "the delete failed"
check "primary: the fold clears the 200 rows of the deletes" test "$(fold)" == 200
catchup primary standby || bail "the standby did not catch up after the fold"
wait
check "standby without feedback: the older snapshot is cancelled by the conflict" grep -q 'conflict with recovery' "$(dir standby)/r1.out"
check "standby: the answer after the fold is the heap's" agree standby

# with feedback: the standby snapshot holds the horizon of the primary back
node_stop standby
node_conf standby "hot_standby_feedback = on"
node_start standby || bail "standby restart failed"
reader "$(dir standby)/r2.out"
# the snapshot of the reader is what the primary was told of (the slot of the standby holds its xmin)
reader_held() {
	local x
	x=$(sql standby "SELECT backend_xmin FROM pg_stat_activity WHERE application_name = 'fm_reader'")
	[[ -n $x && $(sql primary "SELECT xmin FROM pg_replication_slots WHERE slot_name = 'standby'") == "$x" ]]
}
wait_for reader_held 60 || bail "the standby sent no feedback"
sql primary "DELETE FROM fm WHERE id % 100 = 9" >/dev/null || bail "the delete failed"
check "primary: a standby snapshot with feedback holds the fold back" test "$(fold)" == 0
catchup primary standby || bail "the standby did not catch up after the delete"
stop_reader
check "standby with feedback: the older snapshot is not cancelled" bash -c "! grep -q 'conflict with recovery' '$(dir standby)/r2.out'"
# the feedback follows the standby: once its snapshot is gone the primary folds
folded=0
wait_for "folded=\$(fold); [[ \$folded != 0 ]]" 60
check "primary: the fold clears the rows when the snapshot is gone" test "$folded" == 200
catchup primary standby || bail "the standby did not catch up after the second fold"
check "standby: the answers are the heap's and the primary's" test "$(rows standby on)|$(rows standby off)" == "$(rows primary off)|$(rows primary off)"
finish
