#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The workloads suite of the load gate (scripts/load/run.py --suite workloads, scripts/load/suites): hot narrow updates beside analytics, serializable write skew, two-phase commit,
# deep savepoints and held cursors, online DDL of partitioned tables beside an unprivileged lock probe, 32 pooled sessions terminated while they commit, analytics under a tight
# work_mem with cancels, projections and tiering under writes and maintenance, and eight sessions that oversubscribe a pool of four parallel workers; then the cases of Spec 111
# (scripts/load/suites/workloads_next.py): the V6 operators under a tight work_mem, rewrites, splits and merges of a sift_table, upserts against foreign keys and unique indexes,
# evictions of the tier cache, and workers on process numbers above 256; the cases of Spec 114 and 115 (workloads_prod2.py, workloads_prod3.py), the last of which kills the
# server with -9 (LOAD_CRASH) and starts it again (LOAD_RESTART). A cluster of its own: 320 sessions (256 of them idle) and prepared transactions, a small parallel pool.
# One check a case; a case that names a finding (open=Snnn-NN) is a TODO while it fails and fails when it stops.
# Runs on Format 2.0 and Format 1.0 (the cases of Format 2.0 only skip), the release library and Assert-PG.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
. "$TAP_LIB/load.bash"

node_init lw 1 "shared_preload_libraries = 'pg_sift'
autovacuum = off
max_connections = 320
max_prepared_transactions = 16
max_worker_processes = 24
max_parallel_workers = 4
sift.maintenance_naptime = 200
deadlock_timeout = '100ms'
$LOAD_CONF"
node_start lw || bail "start failed"
load_suite lw --suite workloads
done_testing
finish
