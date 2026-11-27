#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# The stress suite of the load gate (scripts/load/run.py --suite stress, scripts/load/suites/workloads_prod5.py, Spec 117): serializable write skew through the three engines
# and the projections of a sift_table, cursors held across commits and maintenance beside spilling cursors, high-cardinality analytics that spill under cancels, databases
# cloned from a template beside busy siblings, MERGE and COPY that skips bad rows beside hybrid readers, access method, type and tablespace changes under pg_dump, and
# temporary and unlogged objects through a kill -9 (LOAD_CRASH, LOAD_RESTART), the last case. A cluster of its own; one check a case; a case that names a finding
# (open=Snnn-NN) is a TODO while it fails and fails when it stops. Runs on Format 2.0 and Format 1.0, the release library and Assert-PG.
. "${SIFT_REPO:?}/test/t/lib/common.bash"
. "$TAP_LIB/load.bash"

node_init ls5 1 "shared_preload_libraries = 'pg_sift'
autovacuum = off
max_connections = 120
max_worker_processes = 24
max_parallel_workers = 4
sift.maintenance_naptime = 200
deadlock_timeout = '100ms'
$LOAD_CONF"
node_start ls5 || bail "start failed"
load_suite ls5 --suite stress
done_testing
finish
