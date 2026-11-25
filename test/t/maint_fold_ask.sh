#!/usr/bin/env bash
# tap: pooled
# The reads of an index with marks in its log that no fold can clear yet (a snapshot older than their deletes is open) do not start a maintenance worker each
# sift.maintenance_naptime: the fold is asked for when marks that it can clear exist, and none cleared a row here. Once the snapshot is gone the next reads ask, and the fold clears them.
. "$(dirname "$0")/lib.bash"

plan 5
node_init fa 75 "autovacuum = off
sift.test_create_format1 = on
sift.test_col_net = off
sift.async_maintenance = on
sift.maintenance_naptime = 200
log_min_messages = debug1"
node_start fa || bail "start failed"
D=$(dir fa)
sql fa "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int PRIMARY KEY, k int, pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO t SELECT g, g % 50, repeat('x', 40) FROM generate_series(1, 20000) g" \
	"CREATE INDEX t_idx ON t USING sift_col (id, k, pad) WITH (track_changes = 'id')" "VACUUM (FREEZE) t" >/dev/null || bail "setup failed"

# the snapshot that holds the horizon back, until the test lets go of advisory lock (0x51F7, 13)
rm -f "$D/locked" "$D/release"
{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"; echo "SELECT count(*) FROM t;"; echo "SELECT pg_advisory_lock(20983, 13);"; echo "\\! touch '$D/locked'"
  echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; echo "COMMIT;"; } | sql fa >/dev/null &
HOLDER=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sql fa "DELETE FROM t WHERE id % 10 = 0" >/dev/null

workers() { grep -c 'background worker "sift merge" .* exited' "$D/postgres.log"; }
reads() { sql fa "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = on" "DO \$\$ DECLARE t0 timestamptz := clock_timestamp(); n bigint;
	BEGIN WHILE clock_timestamp() - t0 < interval '4 seconds' LOOP SELECT count(*) INTO n FROM (SELECT k, count(*), sum(id) FROM t GROUP BY k) x; END LOOP; END \$\$" >/dev/null; }

check "the marks cannot be folded under that snapshot" test "$(sql fa "SELECT sift._test_col_fold_marks('t_idx')")" == 0
w0=$(workers)
reads
w1=$(workers)
diag "workers that ended during 4 seconds of reads under the old snapshot: $((w1 - w0))"
check "reads under a snapshot that holds the marks do not start a worker each naptime" test "$((w1 - w0))" -le 1

# the snapshot ends: the marks can be folded
touch "$D/release"
wait "$HOLDER"
check "the fold clears the 2000 rows of the deletes once it ends" test "$(sql fa "SELECT sift._test_col_fold_marks('t_idx')")" == 2000
check "the index verifies" test -z "$(sql fa "SELECT sift.col_verify('t_idx', true)" 2>&1)"
check "the rows are all there" test "$(sql fa "SELECT count(*) FROM t")" == 18000
finish
