#!/usr/bin/env bash
# A statement that holds a database while its maintenance workers leave lets it go when the backend ends by a FATAL error (pg_terminate_backend), which runs no PG_FINALLY: CREATE DATABASE
# ... TEMPLATE that waits for the lock of its template is terminated, and the workers of the template start again.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 3
node_init df 3 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
sift.async_maintenance = on
sift.maintenance_naptime = 60000"
node_start df || bail "start failed"
real_node df "CREATE DATABASE"
sql df "CREATE DATABASE d1" >/dev/null || bail "setup failed"
d() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir df)/sock" -p "$(port df)" -U postgres -d "$db" "${@/#/--command=}"; }
d d1 "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int) USING sift_table" "INSERT INTO t SELECT g FROM generate_series(1, 1000) g" >/dev/null || bail "setup failed"
workers() { sql df "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND datname = 'd1'"; }
enqueue() { d d1 "SELECT sift._test_maint_enqueue('t'::regclass::oid, 3)" >/dev/null; }

# COMMENT ON DATABASE holds a lock of d1 that CREATE DATABASE ... TEMPLATE d1 waits for, after the hook held d1
tap_tx_hold df comment "BEGIN; COMMENT ON DATABASE d1 IS 'held'"
d postgres "CREATE DATABASE c1 TEMPLATE d1" >/dev/null 2>&1 &
check "the copy of the template waits for its lock" tap_tx_wait_blocked df 1 object
check "the copy is terminated" sql df "SELECT pg_terminate_backend(pid, 10000) FROM pg_stat_activity WHERE query = 'CREATE DATABASE c1 TEMPLATE d1'"
wait $!
tap_tx_release comment
enqueue
check "a worker starts once the terminated copy let the template go" wait_for "[[ \$(workers) == 1 ]]" 20
finish
