#!/usr/bin/env bash
# The extension leaves a database cleanly and its update parts can be applied again. DROP EXTENSION pg_sift ends the maintenance workers attached to the database before it drops the
# objects they read (no worker naps on in a schema that is gone, and the jobs queued for the database are forgotten), a part of sql/6.0 that only defines functions, operators and
# operator classes runs a second time without an error (the header of the parts says each one is safe to run again), and the dump of sift.projections writes no definition of a table that is
# gone.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

REPO=${SIFT_REPO:?}
PARTS=()
for f in "$REPO"/sql/6.0/*.sql; do grep -qE 'pg_extension_config_dump|DROP FUNCTION' "$f" || PARTS+=("$f"); done
out=
plan $((9 + ${#PARTS[@]}))
node_init lc 84 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = on
sift.max_maint_workers = 2
sift.maintenance_naptime = 10000
log_min_messages = warning"
node_start lc || bail "start failed"
SOCK=$(dir lc)/sock
PORT=$(port lc)
dbsql() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$SOCK" -p "$PORT" -U postgres -d "$db" "${@/#/--command=}"; }
setup() {
	sql lc "CREATE DATABASE $1" >/dev/null || bail "create $1 failed"
	dbsql "$1" "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE w (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false)" \
		"CREATE INDEX w_col ON w USING sift_col (id, k, v)" "INSERT INTO w SELECT g, g % 10, g FROM generate_series(1, 1000) g" >/dev/null || bail "setup of $1 failed"
}
ENQ="SELECT sift._test_maint_enqueue('w_col', 3)"
workers_in() { sql lc "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND datname = '$1'"; }
worker_in() { [[ $(workers_in "$1") -ge 1 ]]; }
completed() { dbsql "$1" "SELECT completed FROM sift._test_maint_stats()"; }
rerun() { sed -e 's#MODULE_PATHNAME#$libdir/pg_sift#g' -e '/^\\echo/d' "$1" | PGOPTIONS="-c client_min_messages=warning" psql -X -q -1 -v ON_ERROR_STOP=1 -h "$SOCK" -p "$PORT" -U postgres -d "$2" 2>&1 >/dev/null; }

setup d1
for part in "${PARTS[@]}"; do
	out=$(rerun "$part" d1)
	check "$(basename "$part") runs a second time" test -z "$out"
done

dbsql d1 "$ENQ" >/dev/null
check "a worker attaches to the database that has work" wait_for "worker_in d1" 30
dbsql d1 "$ENQ" "$ENQ" >/dev/null
T0=$SECONDS
out=$(dbsql d1 "DROP EXTENSION pg_sift_test, pg_sift CASCADE" 2>&1)
check "DROP EXTENSION pg_sift of a database with an attached worker succeeds" test -z "$out"
check "no worker is attached to the database when it returns" test "$(workers_in d1)" = 0
check "and it did not wait for the nap of the worker" test $((SECONDS - T0)) -lt 4
check "the log of the server names no schema or relation that the worker lost" test "$(grep -cE 'schema "sift" does not exist|cache lookup failed|could not open relation' "$(dir lc)/postgres.log")" = 0

dbsql d1 "CREATE EXTENSION pg_sift_test CASCADE" "CREATE INDEX w_col ON w USING sift_col (id, k, v)" >/dev/null
C0=$(completed d1)
dbsql d1 "$ENQ" >/dev/null
ran() { [[ $(completed d1) -gt $C0 ]]; }
check "the maintenance of the recreated extension runs" wait_for ran 60

dbsql d1 "CREATE TABLE t (id int, k int)" "CREATE INDEX t_col ON t USING sift_col (id, k)" \
	"SELECT sift.projection_create('p98_live', 't', ARRAY['id', 'k'])" \
	"INSERT INTO sift.projections VALUES ('p98_gone', 'public.gone_table', ARRAY['id'], NULL, NULL, '{}', ARRAY['id'], NULL)" >/dev/null || bail "projection setup failed"
pg_dump -h "$SOCK" -p "$PORT" -U postgres -d d1 -f "$(dir lc)/dump.sql" || bail "pg_dump failed"
check "the dump of sift.projections holds the definition of a table that is there" test "$(grep -c '^p98_live	' "$(dir lc)/dump.sql")" = 1
check "and none of a table that is gone" test "$(grep -c '^p98_gone	' "$(dir lc)/dump.sql")" = 0
out=$(dbsql d1 "ALTER SCHEMA sift RENAME TO sift_v6" 2>&1)
check "the schema of the extension keeps its name" test "$(dbsql d1 "SELECT count(*) FROM pg_namespace WHERE nspname = 'sift'")" = 1
finish
