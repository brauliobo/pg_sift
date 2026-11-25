#!/usr/bin/env bash
# Deletion records of the tombstone log (core/v1_col/tomb/dv.h) on a hot standby: the records reach the standby through the replay of the log pages, the standby reads the deleted rows as gone before
# anything is folded (one record for a unit, tested by the reader), and the fold that clears them on the primary tells the standby first, as the fold of the marks does (col_fold_marks_standby.sh).
. "$(dirname "$0")/lib.bash"

plan 8
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE dv (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL) WITH (autovacuum_enabled = off)" \
	"INSERT INTO dv SELECT i, i % 10, i FROM generate_series(1, 20000) i" \
	"CREATE INDEX dv_c ON dv USING sift_col (id, g, v) WITH (track_changes = 'id')" \
	"VACUUM (FREEZE, ANALYZE) dv" "CHECKPOINT" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = off
max_standby_streaming_delay = 1s
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

Q="SELECT g, count(*), sum(v) FROM dv GROUP BY g ORDER BY g"
rows() { sql "$1" "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $2" "$Q"; }
agree() { test "$(rows "$1" on)" == "$(rows "$1" off)"; }
records() { sql "$1" "SELECT count(*) FROM sift._test_col_log_records('dv_c')"; }
fold() { sql primary "SELECT sift._test_col_fold_marks('dv_c')"; }
reader() {
	sql standby "SET application_name = 'dv_reader'" "BEGIN ISOLATION LEVEL REPEATABLE READ" "SET sift.enable_agg = off" "$Q" "SELECT pg_sleep(60)" "$Q" >"$1" 2>&1 &
	wait_for "[[ \$(sql standby \"SELECT count(*) FROM pg_stat_activity WHERE application_name = 'dv_reader' AND query LIKE '%pg_sleep%'\") == 1 ]]" 60
}
stop_reader() { sql standby "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name = 'dv_reader'" >/dev/null; wait; }

sql primary "DELETE FROM dv WHERE id % 50 = 7" >/dev/null || bail "the delete failed"
catchup primary standby || bail "the standby did not catch up after the delete"
check "primary: the delete wrote deletion records" test "$(records primary)" -gt 0
check "standby: the records arrived with the log pages" test "$(records standby)" == "$(records primary)"
check "standby: the answers before any fold are the heap's" agree standby
check "standby: the unfolded deletes give the primary's answer" test "$(rows standby on)" == "$(rows primary on)"

reader "$(dir standby)/r1.out"
sql primary "DELETE FROM dv WHERE id % 50 = 9" >/dev/null || bail "the second delete failed"
check "primary: the fold clears the rows of both statements" test "$(fold)" == 800
catchup primary standby || bail "the standby did not catch up after the fold"
wait
check "standby without feedback: the older snapshot is cancelled by the conflict" grep -q 'conflict with recovery' "$(dir standby)/r1.out"
check "standby: the answer after the fold is the heap's" agree standby
check "both: no record is left once its rows are cleared" test "$(records primary)|$(records standby)" == "0|0"
finish
