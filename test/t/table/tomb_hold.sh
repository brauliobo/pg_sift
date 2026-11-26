#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The time the rewrite of the tombstone log holds the exclusive lock of the log (the test build logs it) follows the entries appended while the new log is built, not the
# entries of the log: VACUUM with a transaction open that keeps every entry (so the new log is as long as the old), the log in runs on temp files (sift.backend_cache_kb 1 MB),
# at N and 4 N entries; then with the build held at its gate while 300 UPDATEs append. What the hold reads under the lock is counted, not timed (the log line says how many entries it read: those appended meanwhile): a rewrite that read the log again under the lock would read N or 4 N. SIFT_TH_ROWS: N (100000 in a full run; the 1.6 M case of the measurement is 16 x N). SIFT_TH_ROWS: N (100000 in a full run; the 1.6 M case of the measurement is 16 x N).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

N=$(knob SIFT_TH_ROWS 100000 20000)
GATE=7000
PH_TOMB=7
APPENDS=300
# Format 1: the tombstone log and the rewrite that holds its lock; Format 2 keeps its deletes in deletion vectors and has no such log
node_init primary 2 "autovacuum = off
sift.test_create_format1 = on
max_wal_size = 4GB"
node_start primary || bail "primary start failed"
D=$(dir primary)
sql primary "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "setup failed"
plan 6

HOLDER=
hold() { # PHASE
	rm -f "$D/held"
	{ echo "SELECT pg_advisory_lock($((GATE + $1)));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
	HOLDER=$!
	wait_for "[[ -e $D/held ]]" 60 || bail "the holder of the gate did not start"
}
release() {
	sql primary "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null 2>&1
	wait "$HOLDER" 2>/dev/null
}
waiting_at() { [[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted AND objid = $((GATE + $1))") -ge 1 ]]; }
# the last rewrite of the log: "exclusive hold H us, T entries appended meanwhile, K kept"
last_rewrite() { grep "sift tomb rewrite" "$D/postgres.log" | tail -1; }
field() { sed -E "s/.*exclusive hold ([0-9]+) us, ([0-9]+) entries appended meanwhile, ([0-9]+) kept.*/\\$1/" <<<"$(last_rewrite)"; }

# a table of n + 1000 rows: 500 deleted before a snapshot opens (the rewrite drops them) and n under it (it keeps them: the new log is as long as the old)
rewrite_of() { # n: the hold in microseconds
	local n=$1 snap=
	sql primary "DROP TABLE IF EXISTS t" "CREATE TABLE t USING sift_table AS SELECT g AS id, g AS v FROM generate_series(1, $n + 1000) g" "DELETE FROM t WHERE id > $n + 500" >/dev/null
	rm -f "$D/snap"
	{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"; echo "SELECT count(*) FROM t;"; echo "\\! touch '$D/snap'"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
	snap=$!
	wait_for "[[ -e $D/snap ]]" 60 || bail "the snapshot holder did not start"
	sql primary "DELETE FROM t WHERE id <= $n" >/dev/null
	sql primary "SET sift.backend_cache_kb = 1024" "VACUUM t" >/dev/null
	kill "$snap" 2>/dev/null; sql primary "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null 2>&1
	wait "$snap" 2>/dev/null
}

rewrite_of "$N"
a1=$(field 2); k1=$(field 3)
diag "N = $N: hold $(field 1) us, $a1 appended, $k1 kept"
rewrite_of $((4 * N))
a4=$(field 2); k4=$(field 3)
diag "4 N = $((4 * N)): hold $(field 1) us, $a4 appended, $k4 kept"
check "the rewrite kept every entry (the new log is as long as the old)" test "$k1" -ge "$N" -a "$k4" -ge "$((4 * N))"
check "no entry appended meanwhile: the hold reads no entry of a log of N or of 4 N (it reads the entries appended meanwhile, 0)" test "$a1" -eq 0 -a "$a4" -eq 0

# entries appended while the build is held: the hold follows them
sql primary "DROP TABLE IF EXISTS t" "CREATE TABLE t USING sift_table AS SELECT g AS id, g AS v FROM generate_series(1, $((4 * N)) + 1000) g" "VACUUM t" >/dev/null
sql primary "DELETE FROM t WHERE id <= $((4 * N))" >/dev/null
hold $PH_TOMB
sql primary "SET sift.test_phase_gate = $GATE" "SET sift.backend_cache_kb = 1024" "VACUUM t" >/dev/null 2>&1 &
V=$!
check "VACUUM waits in the build of the new log" wait_for "waiting_at $PH_TOMB" 60
for i in $(seq "$APPENDS"); do echo "UPDATE t SET v = v + 1 WHERE id = $((4 * N + 1 + i % 900));"; done | sql primary >/dev/null
release
wait "$V"
check "the VACUUM ended" test $? -eq 0
diag "4 N, $APPENDS appended during the build: hold $(field 1) us, $(field 2) appended"
check "the entries appended meanwhile are the ones the hold moved" test "$(field 2)" -ge "$APPENDS"
check "the table reads its live rows" test "$(sql primary "SELECT count(*) FROM t")" == 1000
finish
