#!/usr/bin/env bash
# The maintenance workers of a database never keep it from being dropped, copied or closed to connections. A worker that cannot connect (ALLOW_CONNECTIONS false) gives its slot back at once and is
# not started again in a loop, and a worker that is attached to a database ends when DROP DATABASE or CREATE DATABASE ... TEMPLATE asks for it (no 55006 "being accessed by other users", whatever the
# nap of the worker).
. "${SIFT_REPO:?}/test/t/lib/common.bash"

out=
plan 9
node_init bg 82 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
sift.async_maintenance = on
sift.max_maint_workers = 2
sift.maintenance_naptime = 10000
log_min_messages = warning"
node_start bg || bail "start failed"
sql bg "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
SOCK=$(dir bg)/sock
PORT=$(port bg)
dbsql() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$SOCK" -p "$PORT" -U postgres -d "$db" "${@/#/--command=}"; }
setup() {
	sql bg "CREATE DATABASE $1" >/dev/null || bail "create $1 failed"
	dbsql "$1" "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE w (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false)" \
		"INSERT INTO w SELECT g, g % 10, g FROM generate_series(1, 1000) g" "CREATE INDEX w_col ON w USING sift_col (id, k, v)" >/dev/null || bail "setup of $1 failed"
}
ENQ="SELECT sift._test_maint_enqueue('w_col', 3)"
worker_in() { [[ $(sql bg "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND datname = '$1'") -ge 1 ]]; }
none_active() { [[ $(sql bg "SELECT workers_active FROM sift_internal.maint_pacer()") == 0 ]]; }
completed() { sql bg "SELECT completed FROM sift._test_maint_stats()"; }
connect_fatals() { grep -c 'not currently accepting connections' "$(dir bg)/postgres.log"; }

setup d3
F0=$(connect_fatals)
C0=$(completed)
D=$(dir bg)
rm -f "$D/ready" "$D/go"
{ echo "\\! touch '$D/ready'"; echo "\\! while [ ! -e '$D/go' ]; do sleep 0.05; done"; echo "$ENQ;"; } | dbsql d3 >/dev/null &
session=$!
wait_for "[[ -e $D/ready ]]" 30 || bail "the session of d3 did not start"
sql bg "ALTER DATABASE d3 ALLOW_CONNECTIONS false" >/dev/null
touch "$D/go"
wait "$session"
check "a worker that cannot connect gives its slot back" wait_for "[[ \$(connect_fatals) -gt $F0 ]] && none_active" 8
sql bg "ALTER DATABASE d3 ALLOW_CONNECTIONS true" >/dev/null
dbsql d3 "$ENQ" >/dev/null
ran() { [[ $(completed) -gt $C0 ]]; }
check "maintenance of the database resumes when it accepts connections again" wait_for ran 60
check "and the closed database was tried a few times, not in a loop" test $(($(connect_fatals) - F0)) -le 4

setup d1
dbsql d1 "$ENQ" >/dev/null
check "a worker attaches to the database that has work" wait_for "worker_in d1" 30
T0=$SECONDS
out=$(sql bg "DROP DATABASE d1" 2>&1)
check "DROP DATABASE of a database with an attached worker succeeds" test -z "$out"
check "and does not wait for the nap of the worker" test $((SECONDS - T0)) -lt 4

setup d2
dbsql d2 "$ENQ" >/dev/null
check "a worker attaches to the template" wait_for "worker_in d2" 30
out=$(sql bg "CREATE DATABASE d2_copy TEMPLATE d2" 2>&1)
check "CREATE DATABASE ... TEMPLATE of a database with an attached worker succeeds" test -z "$out"
check "and the copy has the rows" test "$(dbsql d2_copy "SELECT count(*) FROM w")" = 1000
finish
