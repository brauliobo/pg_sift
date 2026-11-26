#!/usr/bin/env bash
# DROP DATABASE runs no hook for each relation it drops: the slots of the indexes of the database in the registry of the manifests, and the snapshots they hold, are given back by the
# statement itself, so that a server that creates and drops databases does not fill the registry with indexes that are gone. An index of another database keeps its slot.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

out=
plan 4
node_init dd 5 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = off
sift.test_create_format1 = off
sift.shared_cache_kb = 16384"
node_start dd || bail "start failed"
sql dd "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
SOCK=$(dir dd)/sock
PORT=$(port dd)
dbsql() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$SOCK" -p "$PORT" -U postgres -d "$db" "${@/#/--command=}"; }
setup() {
	dbsql "$1" "CREATE TABLE w (id int, k int, v int)" "INSERT INTO w SELECT g, g % 10, g FROM generate_series(1, 2000) g" \
		"CREATE INDEX w_col ON w USING sift_col (id, k, v)" "SET enable_seqscan = off" "SELECT count(*) FROM w WHERE id > 10" >/dev/null || bail "setup of $1 failed"
}
stat() { sql dd "SELECT sift._test_shm_manifest_stat('$1')"; }
setup postgres
base=$(stat slots_valid)
for d in d1 d2; do
	sql dd "CREATE DATABASE $d" >/dev/null || bail "create $d failed"
	dbsql "$d" "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension of $d failed"
	setup "$d"
done
check "the indexes of the new databases are in the registry" test "$(stat slots_valid)" -gt "$base"
out=$(sql dd "DROP DATABASE d1" "DROP DATABASE d2" 2>&1)
check "the databases are dropped" test -z "$out"
check "their slots are given back" test "$(stat slots_valid)" -eq "$base"
check "and the index of the database that stays is still served" test "$(sql dd "SET enable_seqscan = off" "SELECT count(*) FROM w WHERE id > 10" | tail -1)" = 1990
finish
