#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The groups of a COPY into a sift_table are written by the backend and the workers of a parallel context (table/load.c) when the run is 16 MB of rows or more.
# With a streaming standby attached: the load with workers and without give the same TIDs, answers and verified index, and the standby replays the same rows; a COPY in a
# transaction block, with FREEZE and into a partitioned table writes the same rows too; and a load that dies leaves nothing: a worker ended by pg_terminate_backend, a
# worker cancelled, a worker killed with kill -9 (the postmaster restarts the cluster), the statement timeout while the participants write, and the backend killed with kill -9. After
# each the table is empty, VACUUM leaves the file as small as that of an empty table (no orphan group), the same load works and equals the heap twin, and the standby counts as the primary.
# SIFT_LW_ROWS (300000, 200000 quick).
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_LW_ROWS 300000 200000)
node_init primary 2 "wal_level = replica
max_wal_senders = 4
wal_keep_size = 256MB
autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start primary || bail "primary start failed"
D=$(dir primary)
sql primary "CREATE EXTENSION pg_sift" \
	"CREATE TABLE src AS SELECT g AS id, g % 500 AS grp, (g % 997)::numeric(10,2) AS amount, md5(g::text) || md5((g * 7)::text) AS pad FROM generate_series(1, $ROWS) g" \
	"COPY src TO '$D/load.csv' CSV" \
	"CREATE TABLE lt (id int, grp int, amount numeric(10,2), pad text) USING sift_table" \
	"CREATE INDEX lt_bm ON lt USING sift (pad)" >/dev/null || bail "setup failed"
node_clone primary standby 3 "hot_standby_feedback = on
wal_receiver_status_interval = 1s"
node_start standby || bail "standby start failed"
catchup primary standby || bail "the standby did not catch up"

OPTS=("SET max_parallel_maintenance_workers = 2" "SET maintenance_work_mem = '256MB'" "SET sift.table_write_buffer_limit = 2048" "SET sift.test_par_workers = 0")
COPYSQL="COPY lt FROM '$D/load.csv' CSV"
GATE=7000
digest() { sql "$1" "SELECT count(*) || ':' || md5(string_agg(id || ':' || ctid::text || ':' || amount, ',' ORDER BY id)) FROM lt" | tail -1; }
answers() { sql "$1" "SELECT count(*) || ':' || sum(amount) FROM lt WHERE grp = 7" "SELECT count(*) FROM lt WHERE pad @@> 'zzzz'" | tr '\n' ' '; }
twin() { sql primary "SELECT count(*) || ':' || sum(amount) FROM src WHERE grp = 7" | tr '\n' ' '; }
used() { sql primary "SELECT relation_blocks - free_blocks FROM sift.table_space('lt')"; }
workers() { sql primary "SELECT current_setting('sift.test_par_workers')"; }
verified() { [[ -z $(sql "$1" "SELECT sift.verify('lt_bm', true)" 2>&1) ]]; }
reset() { sql primary "TRUNCATE lt" >/dev/null && sql primary "VACUUM lt" >/dev/null; }
same_on_standby() { catchup primary standby && [[ $(digest standby) == "$(digest primary)" ]] && verified standby; }

# the first load: with workers, then without
used0=$(used)
out=$(sql primary "${OPTS[@]}" "$COPYSQL" "SELECT current_setting('sift.test_par_workers')" | tail -1)
check "the load starts workers" test "${out:-0}" -gt 0
with=$(digest primary)
check "the load equals the heap twin: the rows" test "$(sql primary "SELECT count(*) FROM lt")" == "$ROWS"
check "... the answers" test "$(answers primary)" == "$(twin)0 "
check "... and the index verifies" verified primary
check "the standby replays the same rows at the same TIDs and verifies" same_on_standby
reset
out=$(sql primary "${OPTS[@]}" "SET max_parallel_maintenance_workers = 0" "$COPYSQL" "SELECT current_setting('sift.test_par_workers')" | tail -1)
check "the load with no worker starts none" test "$out" == 0
check "... and gives the same rows at the same TIDs as the load with workers" test "$(digest primary)" == "$with"
reset

# a transaction block, COPY FREEZE (a table created in the transaction), a partitioned table
check "a COPY in a transaction block: the same rows" test "$(sql primary "${OPTS[@]}" "BEGIN" "$COPYSQL" "SELECT current_setting('sift.test_par_workers')::int > 0" "COMMIT" | tail -1; digest primary)" == "t
$with"
reset
check "a COPY in a transaction block that rolls back leaves no row" test "$(sql primary "${OPTS[@]}" "BEGIN" "$COPYSQL" "ROLLBACK" "SELECT count(*) FROM lt" | tail -1)" == 0
check "COPY FREEZE into a table created in the transaction: workers, the same rows" test "$(sql primary "${OPTS[@]}" "BEGIN" "CREATE TABLE lf (LIKE lt) USING sift_table" "COPY lf FROM '$D/load.csv' CSV FREEZE" "SELECT current_setting('sift.test_par_workers')::int > 0" "COMMIT" "SELECT count(*) || ':' || sum(amount) FROM lf WHERE grp = 7" | tail -2 | tr '\n' ' ')" == "t $(twin)"
sql primary "DROP TABLE lf" >/dev/null
sql primary "CREATE TABLE lp (id int, grp int, amount numeric(10,2), pad text) PARTITION BY RANGE (id)" \
	"CREATE TABLE lp1 PARTITION OF lp FOR VALUES FROM (0) TO ($((ROWS / 2 + 1))) USING sift_table" \
	"CREATE TABLE lp2 PARTITION OF lp FOR VALUES FROM ($((ROWS / 2 + 1))) TO ($((ROWS + 1))) USING sift_table" >/dev/null
check "COPY into a partitioned table: the same rows" test "$(sql primary "${OPTS[@]}" "COPY lp FROM '$D/load.csv' CSV" "SELECT count(*) || ':' || sum(amount) FROM lp WHERE grp = 7" | tail -1)/$(sql primary "SELECT count(*) FROM lp1 WHERE id > $((ROWS / 2))")" == "$(twin | tr -d ' ')/0"
sql primary "DROP TABLE lp" >/dev/null

# the failures: the participants wait (the lock of sift.test_phase_gate) so that a worker is there to be found
HOLDER=
hold() {
	rm -f "$D/held"
	{ echo "SELECT pg_advisory_lock($((GATE + 4)));"; echo "\\! touch '$D/held'"; echo "SELECT pg_sleep(600);"; } | sql primary >/dev/null 2>&1 &
	HOLDER=$!
	until [[ -e $D/held ]]; do sleep 0.05; done
}
release() {
	sql primary "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()" >/dev/null 2>&1
	wait "$HOLDER" 2>/dev/null
}
find_worker() {
	local v
	for _ in $(seq 600); do
		v=$(sql primary "SELECT pid FROM pg_stat_activity WHERE backend_type = 'parallel worker' AND wait_event_type = 'Lock' LIMIT 1" 2>/dev/null)
		[[ -z $v ]] || { echo "$v"; return 0; }
		kill -0 "$LOADER" 2>/dev/null || return 1
		sleep 0.05
	done
	return 1
}
find_leader() { sql primary "SELECT pid FROM pg_stat_activity WHERE query LIKE 'COPY lt FROM%' AND backend_type = 'client backend' AND pid <> pg_backend_pid() LIMIT 1"; }
restart() {
	local pm=$1
	for _ in $(seq 300); do kill -0 "$pm" 2>/dev/null || break; sleep 0.1; done
	kill -0 "$pm" 2>/dev/null && return 1
	node_start primary && catchup primary standby
}
# after the failure: nothing of the load on either node, the file as that of an empty table, the same load works and equals the twin
recovered() { # LABEL
	check "$1: no row, on the primary and the standby" test "$(sql primary "SELECT count(*) FROM lt")/$(sql standby "SELECT count(*) FROM lt")" == 0/0
	sql primary "VACUUM lt" >/dev/null
	diag "$1: blocks in use $(used) (empty table $used0)"
	check "$1: VACUUM leaves no group of the load: the file is as small as the empty table's" test "$(used)" -le "$((used0 + 8))"
	sql primary "${OPTS[@]}" "$COPYSQL" >/dev/null || { not_ok "$1: the load run again failed"; return 1; }
	check "$1: the load run again equals the heap twin" test "$(answers primary)" == "$(twin)0 "
	check "$1: ... the index verifies" verified primary
	check "$1: ... the standby counts as the primary" same_on_standby
	reset
}
attempt() { # LABEL ACTION [extra SET]: the load waits at the gate; ACTION on a worker (pid in $victim) or the leader
	local label=$1 action=$2 extra=${3:-} err
	hold
	sql primary "${OPTS[@]}" "SET sift.test_phase_gate = $GATE" ${extra:+"$extra"} "$COPYSQL" >"$D/load.out" 2>&1 &
	LOADER=$!
	victim=$(find_worker) || victim=
	check "$label: a worker is writing a group" test -n "$victim"
	leader=$(find_leader)
	PM=$(head -1 "$D/data/postmaster.pid")
	case $action in
		terminate) sql primary "SELECT pg_terminate_backend($victim)" >/dev/null ;;
		cancel) sql primary "SELECT pg_cancel_backend($victim)" >/dev/null ;;
		kill9worker) SIFT_TMP=$D leak_expect 1 'terminated by signal 9'; kill -9 "$victim" ;;
		kill9leader) SIFT_TMP=$D leak_expect 1 'terminated by signal 9'; kill -9 "$leader" ;;
		timeout) ;;
	esac
	wait "$LOADER"; rc=$?
	release
	check "$label: the COPY failed" test "$rc" -ne 0
}
attempt "a worker ended by pg_terminate_backend" terminate
recovered "worker terminated"
attempt "a worker cancelled" cancel
recovered "worker cancelled"
attempt "the statement timeout while the participants write" timeout "SET statement_timeout = '4s'"
recovered "statement timeout"
attempt "a worker killed with kill -9" kill9worker
if restart "$PM"; then ok "the primary restarts and the standby streams again"; recovered "worker killed"; else not_ok "the primary restarts"; fi
attempt "the backend killed with kill -9" kill9leader
if restart "$PM"; then ok "the primary restarts and the standby streams again"; recovered "backend killed"; else not_ok "the primary restarts"; fi
echo "1..$NTEST"
finish
