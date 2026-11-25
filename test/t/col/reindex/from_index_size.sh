#!/usr/bin/env bash
# tap: pooled
# The index a REINDEX that takes its rows from the index it replaces (col/v1/build/from_index.c) leaves is the one a REINDEX from the heap leaves: the same immutable segments (the merge
# that writes them holds two fifths of its memory where a build holds half, so it may cut one segment more), and no more bytes in the file (the segments' streams take their extents as the build's do, so no hole is left between them). Two sizes of a key_cut index of wide
# rows at the default maintenance_work_mem: one segment, and the first size that needs two.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

SIZES=(100000 200000 400000)
plan $((${#SIZES[@]} * 2))

node_init rs 92 "sift.col_build_source = auto
autovacuum = off
max_parallel_workers_per_gather = 0
sift.async_maintenance = off"
node_start rs || bail "start failed"
sql rs "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"

# shape ROWS: "segments bytes" of the index after a REINDEX from the heap, and after one from the old index, and whether that one took its rows from it
shape() {
	sql rs "DROP TABLE IF EXISTS e CASCADE" \
		"CREATE TABLE e WITH (autovacuum_enabled = off) AS SELECT g AS id, 1 + (g::bigint * 7919 % 600)::int AS tenant_id, g % 1000 AS grp, 'l' || g % 2000 AS label, md5(g::text) AS body, md5((g + 1)::text) AS body2, md5((g + 2)::text) AS body3,
			timestamp '2026-01-01' + g * interval '1 second' AS created_at FROM generate_series(1, $1) g" "VACUUM ANALYZE e" >/dev/null || bail "setup of $1 rows failed"
	sql rs "SET max_parallel_maintenance_workers = 0" "CREATE INDEX e_col ON e USING sift_col (id, tenant_id, grp, label, body, body2, body3, created_at) WITH (key_cut = 'tenant_id', track_changes = 'off')" \
		"SET sift.test_col_from_index_off = on" "REINDEX INDEX e_col" \
		"SELECT count(*) FILTER (WHERE kind = 'immutable'), coalesce(sum(bytes) FILTER (WHERE kind = 'immutable'), 0), pg_relation_size('e_col') FROM sift.col_segments('e_col')" \
		"RESET sift.test_col_from_index_off" "REINDEX INDEX e_col" \
		"SELECT count(*) FILTER (WHERE kind = 'immutable'), coalesce(sum(bytes) FILTER (WHERE kind = 'immutable'), 0), pg_relation_size('e_col') FROM sift.col_segments('e_col')" "SELECT current_setting('sift.test_col_from_index_used')" | tr '|\n' '  '
}

for n in "${SIZES[@]}"; do
	read -r hsegs hseg_bytes hbytes osegs oseg_bytes obytes used <<<"$(shape "$n")"
	diag "REINDEX of $n rows: ${hsegs} segments of ${hseg_bytes} bytes (file ${hbytes}) from the heap, ${osegs} of ${oseg_bytes} (file ${obytes}) from the old index"
	check "$n rows: the REINDEX took its rows from the old index and wrote the segments of the one from the heap, or one more" test "$used" == 1 -a "$osegs" -ge "$hsegs" -a "$osegs" -le "$((hsegs + 1))"
	check "$n rows: ... in no more segment bytes" test "$oseg_bytes" -le "$hseg_bytes"
done
finish
