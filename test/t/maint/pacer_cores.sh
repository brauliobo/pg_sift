#!/usr/bin/env bash
# The pacer measures the client backends against the CPUs of the host, not against max_parallel_workers: six running queries with the default 8 parallel workers on a host of many cores are a
# busy server (moderate: P2 and P3 still run, paced) and not a saturated one (throttled: P2 and P3 wait). Needs 16 cores, so that six queries are under three quarters of them.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 2
node_init pc 2 "autovacuum = off
sift.test_create_format1 = off
sift.async_maintenance = on
max_parallel_workers = 8"
node_start pc || bail "start failed"
sql pc "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int) USING sift_table" \
	"INSERT INTO t SELECT g FROM generate_series(1, 1000) g" >/dev/null || bail "setup failed"
sql pc "SELECT sift._test_maint_enqueue('t'::regclass::oid, 3)" >/dev/null
wait_for "[[ \$(sql pc 'SELECT pending FROM sift._test_maint_stats()') == 0 ]]" 30 || bail "the worker that samples the pacer did not run"

for i in $(seq 6); do sql pc "SELECT pg_sleep(6)" >/dev/null & done
wait_for "[[ \$(sql pc \"SELECT count(*) FROM pg_stat_activity WHERE query = 'SELECT pg_sleep(6)' AND state = 'active'\") == 6 ]]" 10 || bail "the six queries did not start"
modes=$(for _ in $(seq 8); do sql pc "SELECT pacer_mode FROM sift_internal.maint_pacer()"; sleep 0.3; done | sort -u | paste -sd' ' -)
wait
out=$modes
scales 8 "six running queries make the pacer moderate and never throttle it ($modes)" eval '[[ $modes == *moderate* && $modes != *throttled* && $modes != *deferred* ]]'
check "the pacer is full again once they end" wait_for "[[ \$(sql pc 'SELECT pacer_mode FROM sift_internal.maint_pacer()') == full ]]" 10
