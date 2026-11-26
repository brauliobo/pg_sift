#!/usr/bin/env bash
# The maintenance workers of a database leave before ALTER DATABASE ... RENAME TO and ALTER DATABASE ... SET TABLESPACE look for the sessions that use it, as they do for DROP DATABASE:
# a worker that naps in the database must not make the statement fail with "being accessed by other users". The schema of the extension is not renamed, preloaded or not.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
node_init dr 3 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
sift.async_maintenance = on
sift.maintenance_naptime = 60000"
node_start dr || bail "start failed"
real_node dr "ALTER DATABASE"
mkdir -p "$(dir dr)/ts"
sql dr "CREATE DATABASE d1" "CREATE TABLESPACE ts LOCATION '$(dir dr)/ts'" >/dev/null || bail "setup failed"
d() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir dr)/sock" -p "$(port dr)" -U postgres -d "$db" "${@/#/--command=}"; }
d d1 "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int) USING sift_table" "INSERT INTO t SELECT g FROM generate_series(1, 1000) g" >/dev/null || bail "setup failed"
workers() { sql dr "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND datname = 'd1'"; }

# a worker is attached to d1 and naps there
d d1 "SELECT sift._test_maint_enqueue('t'::regclass::oid, 3)" >/dev/null
check "a maintenance worker is attached to d1" wait_for "[[ \$(workers) == 1 ]]" 20
check "ALTER DATABASE RENAME drains the worker and renames" sql dr "ALTER DATABASE d1 RENAME TO d1r"
d d1r "SELECT sift._test_maint_enqueue('t'::regclass::oid, 3)" >/dev/null
check "a worker is attached again" wait_for "[[ \$(sql dr \"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND datname = 'd1r'\") == 1 ]]" 20
check "ALTER DATABASE SET TABLESPACE drains the worker" sql dr "ALTER DATABASE d1r SET TABLESPACE ts"
check "the schema of the extension is not renamed" test "$(d d1r "ALTER SCHEMA sift RENAME TO sift_broken" 2>&1 | grep -c 'cannot rename schema')" == 1

# a server that does not preload the library: the session that loaded it still guards the schema
node_init dp 5 "autovacuum = off"
node_start dp || bail "start failed"
check "without preloading, the schema of the extension is not renamed either" \
	test "$(sql dp "CREATE EXTENSION pg_sift" "SELECT count(*) FROM sift.upgrade_check()" "ALTER SCHEMA sift RENAME TO sift_broken" 2>&1 | grep -c 'cannot rename schema')" == 1
