#!/usr/bin/env bash
# The merge of a parallel build, by the participants, each a slice of the key range (col/build_par.c, sift_col_merge_slice): one slice for each partition the
# output is cut into (here of 256 kB, from segments of 64 kB). The index is the one the merge of a single process makes in what a query sees: the same rows, the units
# a window of dates reads and the rows it scans, a segment for each slice with no unit number in two of them, the file the live bytes, and the answers the table's.
# It leaves nothing when it stops: a slice that PANICs after writing its segment and before the directory switch takes the server down, and crash recovery finds no
# index (the CREATE INDEX never committed); a statement cancelled in the middle of the merge (a slice sleeps) rolls the build back.
. "$(dirname "$0")/lib.bash"

plan 15
node_init mc 22 "autovacuum = off
max_worker_processes = 16
max_parallel_workers = 8"
node_start mc || bail "start failed"
# 12 members of 20,000 rows (more than a unit holds), dates scattered over the heap
sql mc "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE t (id int, dim_b_id int, day date, v int, pad text) WITH (fillfactor = 20, autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, repeat('x', 60) FROM generate_series(1, 240000) g" \
	"VACUUM (FREEZE, ANALYZE) t" "ALTER TABLE t SET (parallel_workers = 4)" >/dev/null || bail "setup failed"
OPTS="SET max_parallel_maintenance_workers = 4"
SMALL="SET min_parallel_table_scan_size = 0"
PART="SET sift.col_auto_merge_partition_kb = 256"
SEG="SET sift.test_col_segment_kb = 64"
CREATE="CREATE INDEX t_idx ON t USING sift_col (dim_b_id, day, v) WITH (key_cut = 'dim_b_id, day')"
WINDOW="SELECT dim_b_id, count(*), sum(v) FROM t WHERE day >= date '2022-03-01' AND day < date '2022-04-01' GROUP BY dim_b_id"
has_index() { sql mc "SELECT count(*) FROM pg_class WHERE relname = 't_idx'"; }
build() { sql mc "$@" "$OPTS" "$SMALL" "$PART" "$SEG" "$CREATE" >/dev/null; }
# segments, units, rows, (unused), the file over the live bytes in percent
shape() { sql mc "SELECT i.segments, i.units, i.rows, (SELECT count(*) FROM sift.col_segments('t_idx') a JOIN sift.col_segments('t_idx') b ON a.gen < b.gen AND a.kind = 'immutable' AND b.kind = 'immutable' AND a.sb_lo <= b.sb_hi AND b.sb_lo <= a.sb_hi), (100 * pg_relation_size('t_idx') / i.bytes) FROM sift.col_index_info('t_idx') i" | tr '|' ' '; }
# the units a window of dates reads and the rows it scans
window() { sql mc "SET max_parallel_workers_per_gather = 0" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $WINDOW" | sed -n -e 's/.*Units: \([0-9]*\) read.*/\1/p' -e 's/.*Rows: \([0-9]*\) scanned.*/\1/p' | tr '\n' ' '; }

SIFT_TMP=$(dir mc) leak_expect 1 'PANIC:'
SIFT_TMP=$(dir mc) leak_expect 1 'terminated by signal 6'
panics() { ! sql mc "SET sift.test_col_merge_panic = on" "$OPTS" "$SMALL" "$PART" "$SEG" "$CREATE" >/dev/null 2>&1; }
check "a slice of the merge PANICs after it wrote its segment" panics
for _ in $(seq 300); do [[ -f $(dir mc)/data/postmaster.pid ]] || break; sleep 0.1; done
check "the server restarts through crash recovery" node_start mc
check "the index of the build that stopped does not exist" test "$(has_index)" == 0

cancelled() {
	local out
	out=$(sql mc "SET sift.test_col_refresh_sleep_ms = 3000" "SET statement_timeout = 2000" "$OPTS" "$SMALL" "$PART" "$SEG" "$CREATE" 2>&1) || true
	grep -q "canceling statement" <<<"$out"
}
check "a cancel in the middle of the merge ends the build" cancelled
check "... and leaves no index" test "$(has_index)" == 0

# the participants merge only when the output is cut into 8 partitions or more: fewer are written by one process in less CPU and WAL
slices_of() { sql mc "DROP INDEX IF EXISTS t_idx" "$@" "$OPTS" "$SMALL" "$SEG" "$CREATE" "SHOW sift.test_col_merge_slices" | tail -n 1; }
check "an output of many partitions is merged by the participants (8 slices or more)" test "$(slices_of "$PART")" -ge 8
check "an output of fewer than 8 partitions is merged by one process" test "$(slices_of "SET sift.col_auto_merge_partition_kb = 65536")" == 0
sql mc "DROP INDEX IF EXISTS t_idx" >/dev/null
build "SET sift.test_col_merge_workers = 0" || bail "the build with one merging process failed"
read -r OSEG OUNIT OROWS OOVER OFILE <<<"$(shape)"
OWIN=$(window)
sql mc "DROP INDEX t_idx" >/dev/null
build || bail "the build with slices failed"
read -r SSEG SUNIT SROWS SOVER SFILE <<<"$(shape)"
SWIN=$(window)
check "the index verifies" test -z "$(sql mc "SELECT sift.col_verify('t_idx', true)" 2>&1)"
check "a segment for each slice (the sizes of the segments of the participants, so the partitions, differ a little between builds)" test "$SSEG" -ge 2
check "the same rows" test "$SROWS" == "$OROWS"
check "the segments are not fragmented (the units of a slice are numbered after those of the one before): nothing is left to rewrite" test "$(sql mc "SELECT coalesce(sift.col_maintenance_due('t_idx', false), 'none')")" == none
check "the units are those of one process (+ one for each boundary)" test "$SUNIT" -le "$((OUNIT + SSEG))" -a "$SUNIT" -ge "$((OUNIT - SSEG))"
check "a window of dates reads the same units and scans the same rows" test "$SWIN" == "$OWIN"
check "the file is the live bytes (at most 110%)" test "$SFILE" -le 110
check "it answers as the heap does" test "$(sql mc "SET enable_seqscan = off" "SELECT count(*), sum(v) FROM t WHERE dim_b_id = 7")" == "$(sql mc "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*), sum(v) FROM t WHERE dim_b_id = 7")"
finish
