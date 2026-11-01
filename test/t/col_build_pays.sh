#!/usr/bin/env bash
# When a parallel build pays (col/build_par.c, build_pays). Without key_cut the participants' segments are the index and the scan is shared; with key_cut their segments overlap
# and the merge that follows rewrites every row, which only pays when it is made in slices (3 or more: the output is cut into partitions, col/maint/merge.c), so a key_cut index
# whose merge would be made by one process is built by one (no worker launched).
. "$(dirname "$0")/lib.bash"

plan 6
node_init mc 22 "autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start mc || bail "start failed"
# one key of 240,000 distinct values, uniform and not in the order of the heap
sql mc "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (id int, ts bigint, v int, pad text) WITH (fillfactor = 20, autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, hashint8extended(g, 1) & 4294967295, g % 100, repeat('x', 60) FROM generate_series(1, 240000) g" \
	"VACUUM (FREEZE, ANALYZE) t" "ALTER TABLE t SET (parallel_workers = 4)" >/dev/null || bail "setup failed"
OPTS="SET max_parallel_maintenance_workers = 4"
SMALL="SET min_parallel_table_scan_size = 0"
PAYS="SET sift.test_col_parallel_always = off"   # the tests of the participants build small tables in parallel whatever it costs
# the workers launched and the slices of the merge of a build with the options in $2 ($1: SET statements)
built() { sql mc "DROP INDEX IF EXISTS t_idx" "$OPTS" "$SMALL" "$PAYS" "SET sift.test_col_segment_kb = 64" "$1" "CREATE INDEX t_idx ON t USING sift_col (ts, v) WITH (key_cut = $2)" "SHOW sift.test_col_workers" "SHOW sift.test_col_merge_slices" | tail -n 2 | tr '\n' ' '; }
check "key_cut, a merge that one process makes (no partitions): the build is made by one" test "$(built "SET sift.col_auto_merge_partition_kb = 0" "'ts'")" == "0 0 "
check "no key_cut: the participants build it" test "$(built "SET sift.col_auto_merge_partition_kb = 0" "'off'" | cut -d' ' -f1)" -ge 1
check "key_cut, a merge in 3 slices or more: the participants build it and merge it" test "$(built "SET sift.col_auto_merge_partition_kb = 256" "'ts'" | cut -d' ' -f2)" -ge 3
check "... with workers" test "$(built "SET sift.col_auto_merge_partition_kb = 256" "'ts'" | cut -d' ' -f1)" -ge 1
check "the index verifies" test -z "$(sql mc "SELECT sift.col_verify('t_idx', true)" 2>&1)"
check "it answers as the heap does" test "$(sql mc "SET enable_seqscan = off" "SELECT count(*), sum(v) FROM t WHERE ts < 1000000000")" == "$(sql mc "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(v) FROM t WHERE ts < 1000000000")"
finish
