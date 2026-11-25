#!/usr/bin/env bash
# One database that always has maintenance work does not keep another from getting a worker: with one worker allowed (sift.max_maint_workers = 1) and requests of a hot database that never run out,
# the request of a quiet database is still served within a few naps.
. "$(dirname "$0")/lib.bash"

out=
plan 3
node_init bf 83 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = on
sift.max_maint_workers = 1
sift.maintenance_naptime = 1000
log_min_messages = warning"
node_start bf || bail "start failed"
sql bf "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
SOCK=$(dir bf)/sock
PORT=$(port bf)
D=$(dir bf)
dbsql() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$SOCK" -p "$PORT" -U postgres -d "$db" "${@/#/--command=}"; }
for db in hot cold; do
	sql bf "CREATE DATABASE $db" >/dev/null || bail "create $db failed"
	dbsql $db "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE w (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false)" \
		"CREATE INDEX w_col ON w USING sift_col (id, k, v)" "INSERT INTO w SELECT g, g % 10, g FROM generate_series(1, 1000) g" >/dev/null || bail "setup of $db failed"
done
ENQ="SELECT sift._test_maint_enqueue('w_col', 3)"
{
	for _ in $(seq 1 30000); do
		[[ -e $D/stop ]] && break
		dbsql hot "$ENQ" >/dev/null
	done
} &
writer=$!
completed() { sql bf "SELECT completed FROM sift._test_maint_stats()"; }
busy() { [[ $(completed) -ge 3 ]]; }
check "the worker serves the hot database" wait_for busy 30
C0=$(completed)
dbsql cold "$ENQ" >/dev/null
served() { [[ $(sql bf "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND datname = 'cold'") -ge 1 ]]; }
check "the request of the quiet database gets a worker while the hot one keeps asking" wait_for served 20
check "the hot database was still asking" kill -0 "$writer"
touch "$D/stop"
wait "$writer"
finish
