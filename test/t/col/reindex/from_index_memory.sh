#!/usr/bin/env bash
# tap: pooled
# The memory of the REINDEX of a key_cut sift_col index that takes its rows from the index it replaces (col/v1/build/from_index.c), in exact kilobytes (sift.test_col_build_peak_kb: what
# the build held at its peak above what the backend started it with) at three sizes, beside the same REINDEX built from the heap (sift.test_col_from_index_off). The floors of the
# budgets are lowered (sift.test_limits) so that maintenance_work_mem is what cuts the rows, as it is on a table of hundreds of millions of rows at the default. The build from the old
# index writes the partitions the writer's memory allows, and reads each input one unit at a time: it holds no more than the build from the heap at any size, and stays under
# maintenance_work_mem plus a constant.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

SIZES=(50000 100000 200000)
MB=1024
MWM=8
plan 8

node_init cm 91 "sift.test_create_format1 = on
sift.col_build_source = auto
autovacuum = off
max_parallel_workers_per_gather = 0
sift.async_maintenance = off"
node_start cm || bail "start failed"
sql cm "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

# peaks ROWS: the kB the REINDEX from the heap and the REINDEX from the old index held at their peak, and whether the second took its rows from the index
peaks() {
	sql cm "DROP TABLE IF EXISTS e CASCADE" \
		"CREATE TABLE e WITH (autovacuum_enabled = off) AS SELECT g AS id, g AS k_hi, g % 5000 AS k_mid, g % 10 AS k_lo, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] AS cat,
			'name' || (g::bigint * 7919) % 1000000 AS txt, ((g::bigint * 7919) % 29500)::numeric / 100 AS amount, timestamp '2025-01-01' + (g::bigint * 311 % 31536000) * interval '1 second' AS ts,
			repeat('x', 400) AS p FROM generate_series(1, $1) g" "VACUUM ANALYZE e" >/dev/null || bail "setup of $1 rows failed"
	sql cm "SET max_parallel_maintenance_workers = 0" "SET maintenance_work_mem = '${MWM}MB'" \
		"CREATE INDEX e_col ON e USING sift_col (k_hi, k_mid, k_lo, cat, txt, amount, ts) WITH (key_cut = 'k_mid', track_changes = 'off')" \
		"SET sift.test_col_from_index_off = on" "REINDEX INDEX e_col" "SELECT current_setting('sift.test_col_build_peak_kb')" \
		"RESET sift.test_col_from_index_off" "REINDEX INDEX e_col" "SELECT current_setting('sift.test_col_build_peak_kb'), current_setting('sift.test_col_from_index_used')" |
		tr '|\n' '  '
}

for n in "${SIZES[@]}"; do
	read -r heap from used <<<"$(peaks "$n")"
	diag "REINDEX of $n rows at ${MWM} MB: ${heap} kB from the heap, ${from} kB from the old index"
	check "$n rows: the REINDEX took its rows from the old index and held at most what the one from the heap did" test "$used" == 1 -a "$from" -le "$heap"
	((n == SIZES[0])) || true
done
# the rows of the write buffers, decoded before the merge takes them (scope col_from_index): exact bytes at three sizes, 260 bytes a row at the most and none for the rows of the index
over=0
for b in 2000 8000 30000; do
	sql cm "DROP TABLE IF EXISTS w CASCADE" "CREATE TABLE w WITH (autovacuum_enabled = off) AS SELECT g AS id, g % 5000 AS k_mid, 'name' || g AS txt, repeat('x', 200) AS p FROM generate_series(1, 20000) g" \
		"CREATE INDEX w_col ON w USING sift_col (id, k_mid, txt) WITH (key_cut = 'k_mid', track_changes = 'off')" \
		"INSERT INTO w SELECT g, g % 5000, 'name' || g, repeat('x', 200) FROM generate_series(20001, 20000 + $b) g" >/dev/null
	peak=$(sql cm "SET maintenance_work_mem = '${MWM}MB'" "REINDEX INDEX w_col" "SELECT peak FROM sift._test_mem_highwater(true) WHERE scope = 'col_from_index'" | tail -n 1)
	diag "col_from_index scope for $b buffered rows: $peak bytes"
	((peak <= 260 * b + 256 * 1024)) || over=1
done
check "the decoded write buffers hold at most 260 bytes a buffered row" test "$over" -eq 0
check "the largest holds under maintenance_work_mem plus 4 MB" test "$from" -le "$((MWM * MB + 4 * MB))"
check "the index passes its check" test "$(sql cm "SELECT sift.col_verify('e_col', true)::text")" == ""
check "the answers are the table's" test "$(sql cm "SET sift.enable_agg = on" "SET enable_seqscan = off" "SELECT count(*), sum(k_hi) FROM e WHERE k_mid >= 0")" == "$(sql cm "SET sift.enable_agg = off" "SELECT count(*), sum(k_hi) FROM e")"
check "the partitions are cut by the writer's memory: more than one segment" test "$(sql cm "SELECT count(*) FROM sift.col_segments('e_col') WHERE kind = 'immutable'")" -gt 1
finish
