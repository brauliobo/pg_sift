#!/usr/bin/env bash
# The background maintenance without shared_preload_libraries (EXT-11, spec 26 section 3.1): the table of the queue is made in the dynamic shared memory registry by the first
# backend that needs it, and the backend that enqueues a request starts the dynamic worker itself; there is no launcher. An injection point (PostgreSQL built with
# --enable-injection-points: scripts/pg-assert.sh --inject) holds the worker, so nothing here depends on timing:
#   a worker starts and holds a slot (sift.maint_pacer) and a claim (sift.maint_queue); terminating it gives both back and the request waits in the queue;
#   the next trigger starts a new worker that drains the queue, and it leaves after its idle naps with no slot held;
#   with no worker slot to spare (max_worker_processes = 2) nothing fails: the request waits, sift.maintain() runs a step in the session, the server stays up.
. "$(dirname "$0")/lib.bash"

pg_config --configure | grep -q -- --enable-injection-points || { echo "1..0 # SKIP PostgreSQL without injection points (scripts/pg-assert.sh --inject)"; exit 0; }
((PG_MAJOR >= 18)) || { echo "1..0 # SKIP the wait of an injection point is named from PostgreSQL 18"; exit 0; }

POINT=sift-maint-f2-after-table-lock
plan 12
node_init zp 27 "shared_preload_libraries = ''
autovacuum = off
sift.async_maintenance = on
sift.max_maint_workers = 2
sift.maintenance_naptime = 500
max_worker_processes = 16"
node_start zp || bail "start failed"
D=$(dir zp)
sql zp "CREATE EXTENSION pg_sift" "CREATE EXTENSION injection_points" \
	"CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 100) FROM generate_series(1, 100000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) facts" >/dev/null || bail "setup failed"

churn() {
	sql zp "UPDATE facts SET amount = amount + 1 WHERE id % 20 = 1" >/dev/null
	sql zp "SET max_parallel_workers_per_gather = 0" "SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind" >/dev/null 2>&1
}
workers() { sql zp "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'"; }
launchers() { sql zp "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'sift maintenance launcher'"; }
waiting() { sql zp "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint' AND wait_event = '$POINT'"; }
pacer() { sql zp "SELECT $1 FROM sift_internal.maint_pacer()"; }
queue() { sql zp "SELECT count(*) FROM sift_internal.maint_queue() WHERE $1"; }
await_worker() { local n; for n in $(seq 600); do churn; [[ $(waiting) -gt 0 ]] && return 0; sleep 0.2; done; return 1; }
crash_lines() { grep -cE 'TRAP|PANIC|terminated by signal|server process .* was terminated' "$D/postgres.log"; }
release() { sql zp "SELECT injection_points_detach('$POINT')" >/dev/null; while [[ $(waiting) -gt 0 ]]; do sql zp "SELECT injection_points_wakeup('$POINT')" >/dev/null; sleep 0.1; done; }

check "pg_sift is not preloaded" test -z "$(sql zp "SHOW shared_preload_libraries")"
sql zp "SELECT injection_points_attach('$POINT', 'wait')" >/dev/null
check "a churn starts a dynamic worker with no launcher" await_worker
check "no launcher runs" test "$(launchers)" == 0
check "the worker holds one slot of two" test "$(pacer "workers_active || '/' || workers_max")" == 1/2
check "its request is running in the queue" test "$(queue "queued = false AND pid IS NOT NULL")" == 1

sql zp "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE backend_type = 'sift maintenance'" >/dev/null
check "a terminated worker gives back its slot and its claim" wait_for "[[ \$(pacer workers_active) == 0 && \$(queue 'queued = false') == 0 ]]" 30
check "and its request waits in the queue" test "$(queue "queued = true")" -ge 1

release
churn
check "the next trigger starts a worker that drains the queue" wait_for "[[ \$(queue true) == 0 ]]" 120
check "the worker leaves after its idle naps and holds no slot" wait_for "[[ \$(workers) == 0 && \$(pacer workers_active) == 0 ]]" 30

node_stop zp
node_conf zp "max_worker_processes = 2"
node_start zp || bail "restart with max_worker_processes = 2 failed"
churn
check "with no worker slot to spare no worker runs and nothing fails" test "$(workers)" == 0
check "sift.maintain() runs a step in the session" test "$(sql zp "SELECT count(*) FROM sift.maintain('facts_idx')")" == 1
check "no crash, no leak" test "$(crash_lines)" == 0
finish
