#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The load gate (scripts/load/run.py, cases.py, fixtures.sql): sift under concurrent writers, readers, pooled sessions, tenants, maintenance and faults on one cluster, judged by
# counts that do not depend on the host: the rows committed are the rows kept, the index paths answer what the heap answers, sift.check, the quarantine and the server log stay clean,
# and the contention ceilings are shares of samples and ratios of a p99 under load to the same p99 alone. One LOAD line a case (a comment the job log keeps) and one check each; a case
# that names a defect (open=LC-NN) is a TODO while it fails its ceilings and fails when it stops. The kill -9 case runs last (crash, restart); a case that crashes the
# server (restart_after_crash is off) gets a fresh start, so the cases after it run on a clean cluster. LOAD_ONLY=RE runs the cases whose name
# matches, LOAD_ARGS=--determinism (--calibrate) the other modes of the runner. Runs on Format 2.0 and Format 1.0 (the cases of Format 2.0 only skip), the release library and Assert-PG.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
. "$TAP_LIB/load.bash"

node_init lg 1 "shared_preload_libraries = 'pg_sift'
autovacuum = off
max_connections = 80
max_worker_processes = 24
max_parallel_workers = 8
sift.maintenance_naptime = 200
deadlock_timeout = '100ms'
$LOAD_CONF"
node_start lg || bail "start failed"
load_suite lg
done_testing
finish
