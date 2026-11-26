#!/usr/bin/env bash
# The launcher of the background maintenance takes a reload (pg_reload_conf) of sift.max_maint_workers without a restart: a server started with the workers off (0) starts them
# once the setting is raised and reloaded, and its queue drains.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 4
node_init sr 2 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = on
sift.max_maint_workers = 0"
node_start sr || bail "start failed"
sql sr "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int) USING sift_table" \
	"INSERT INTO t SELECT g FROM generate_series(1, 1000) g" >/dev/null || bail "setup failed"
stat() { sql sr "SELECT $1 FROM sift._test_maint_stats()"; }

check "with the workers off a request is refused" test "$(sql sr "SELECT sift._test_maint_enqueue('t'::regclass::oid, 3)")" == unavailable
sql sr "ALTER SYSTEM SET sift.max_maint_workers = 2" >/dev/null
pg_ctl -D "$(dir sr)/data" reload >/dev/null
check "a session sees the reloaded setting" wait_for "[[ \$(sql sr 'SHOW sift.max_maint_workers') == 2 ]]" 10
check "the request is queued" test "$(sql sr "SELECT sift._test_maint_enqueue('t'::regclass::oid, 3)")" == queued
check "the launcher starts a worker for it and the queue drains" wait_for "[[ \$(stat spawned) -ge 1 && \$(stat pending) == 0 ]]" 20
