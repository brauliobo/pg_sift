#!/usr/bin/env bash
# The background pass of a sift_table is weighed against the scans the table has had, and those are a count in its metapage (core/reads.c), not the numscans of pg_stat:
# pg_stat_reset(), a clean restart and a crash leave the decision as it was. A table nobody has read asks for no pass.
. "$(dirname "$0")/lib.bash"
plan 6

ROWS=$(knob SIFT_TDS_ROWS 30000 20000)
node_init td 83 "autovacuum = off
sift.async_maintenance = on
sift.maintenance_naptime = 200ms
sift.log_min_duration = 0
max_parallel_maintenance_workers = 0"
node_start td || bail "start failed"
sql td "CREATE EXTENSION pg_sift" >/dev/null || bail "setup failed"
LOG=$(dir td)/postgres.log
FINISHED="table maintenance worker finished"

# a table settled into column segments with 40% of its rows deleted by a session that asks for nothing, then 50 scans (38 of a table dirty throughout cost what a pass does)
prepare() {
	sql td "DROP TABLE IF EXISTS $1" "CREATE TABLE $1 (id int, v bigint, t text) USING sift_table WITH (autovacuum_enabled = false)" "SELECT sift.table_set_option('$1', 'write_buffer_limit', '2048kB')" \
		"INSERT INTO $1 SELECT g, g * 3, 'text ' || md5(g::text) FROM generate_series(1, $ROWS) g" "VACUUM $1" >/dev/null || bail "prepare $1 failed"
	sleep 0.5   # the VACUUM closes a buffer too and may have asked for a pass: a request inside the naptime of that one is not made
	sql td "SELECT sift.table_set_option('$1', 'auto_defrag', 'off')" "DELETE FROM $1 WHERE id % 10 < 4" "SELECT sift.table_set_option('$1', 'auto_defrag', 'on')" >/dev/null
}
scan() { sql td "SELECT sift.table_set_option('$1', 'auto_defrag', 'off')$(printf "; SELECT count(*) FROM $1%.0s" $(seq "$2"))" "SELECT sift.table_set_option('$1', 'auto_defrag', 'on')" >/dev/null; }
# a read that asks for the pass, when the scans so far pay for it
read_once() { sleep 0.5; sql td "SELECT count(*) FROM $1" >/dev/null; }
# the log from byte $1 holds the end of a pass
passed() { tail -c +"$1" "$LOG" | grep -q "$FINISHED"; }
asked() { wait_for "passed $1" 20; }
quiet() { sleep 3; ! passed "$1"; }
mark() { echo $(($(wc -c <"$LOG" | tr -d " ") + 1)); }

prepare idle
off=$(mark); read_once idle
check "a table nobody has read asks for no pass" quiet "$off"

prepare reset
scan reset 50
sql td "SELECT pg_stat_force_next_flush()" "SELECT pg_stat_reset()" >/dev/null
check "pg_stat_reset() zeroed the scans the statistics hold" test "$(sql td "SELECT seq_scan FROM pg_stat_user_tables WHERE relname = 'reset'" | tail -n 1)" -eq 0
off=$(mark); read_once reset
check "and the pass is still asked for" asked "$off"

prepare restart
scan restart 50
wait_for "wal_flushed td" 30
node_stop td; node_start td || bail "restart failed"
off=$(mark); read_once restart
check "a clean restart leaves the decision as it was" asked "$off"

prepare crashed
scan crashed 50
sleep 1
crash td; node_start td || bail "restart after the crash failed"
off=$(mark); read_once crashed
check "a crash leaves it as it was" asked "$off"

check "the rows that were not deleted are all there" test "$(sql td "SELECT count(*) FROM crashed" | tail -n 1)" -eq $((ROWS * 6 / 10))
finish
