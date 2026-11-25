#!/usr/bin/env bash
# The adoption of the partitions of a key-cut index (col/v1/maint/adopt.c) replays on a hot standby: the primary records them with one directory update, the standby replays it to the same partitions
# with the same bounds, reads the same segments and answers as the heap does. Both nodes run with wal_consistency_checking = generic, so that the standby compares every page it replays with the
# image the primary logged.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

BASE=("SET sift.test_col_segment_kb = 1024" "SET sift.test_col_partition_kb = 512" "SET max_parallel_maintenance_workers = 0" "SET sift.async_maintenance = off")
plan 5
node_init primary 2 "$WAL_CONF
wal_level = replica
max_wal_senders = 4
wal_consistency_checking = 'generic'
autovacuum = off"
node_start primary || bail "primary start failed"
sql primary "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE c (id int PRIMARY KEY, dim int NOT NULL, day date NOT NULL, v int, w int) WITH (autovacuum_enabled = false)" \
	"INSERT INTO c SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 400000) g" \
	"VACUUM (FREEZE, ANALYZE) c" "${BASE[@]}" "SET sift.test_col_record_partitions = off" "CREATE INDEX c_idx ON c USING sift_col (id, dim, day, v, w) WITH (key_cut = 'dim, day')" >/dev/null || bail "setup failed"
node_clone primary standby 3 "wal_consistency_checking = 'generic'
hot_standby_feedback = on"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

PARTS="SELECT count(*) FROM sift._test_dir_parts('c_idx')"
LIST="SELECT coalesce(string_agg(p.first || ':' || p.n || ':' || p.lineage || ':' || b.lo || ':' || b.rows_from_lo || b.rows_below_next, ' ' ORDER BY p.first), '') FROM sift._test_dir_parts('c_idx') p JOIN sift._test_col_part_bounds('c_idx') b USING (first)"
SEGS="SELECT coalesce(string_agg(gen || ':' || units || ':' || bytes || ':' || rows, ',' ORDER BY gen), '') FROM sift.col_segments('c_idx') WHERE kind = 'immutable'"
ANS="SELECT string_agg(r::text, ';') FROM (SELECT dim, count(*), sum(v) FROM c GROUP BY 1 ORDER BY 1) r"
check "the index is in partitions that no step recorded, on both nodes" test "$(sql primary "$PARTS")|$(sql standby "$PARTS")" == "0|0"
segs=$(sql primary "$SEGS")
adopted=$(sql primary "${BASE[@]}" "SELECT sift._test_col_adopt('c_idx')" | tail -n 1)
catchup primary standby || bail "the standby did not catch up"
n=$(sql primary "$PARTS")
check "the primary records $n partitions, the standby replays the same ones with the same bounds" test "$adopted|$([[ $n -gt 4 ]] && echo many)|$(sql standby "$LIST")" == "t|many|$(sql primary "$LIST")"
check "no segment changed on either node" test "$(sql primary "$SEGS")|$(sql standby "$SEGS")" == "$segs|$segs"
out="standby $(sql standby "$LIST" | cut -c1-200)"
check "every partition of the standby is within its bounds, and its answers are the heap's" \
	test "$(sql standby "SELECT count(*) FILTER (WHERE rows_from_lo AND rows_below_next) = count(*) FROM sift._test_col_part_bounds('c_idx')")|$(sql standby "SET sift.enable_agg = on" "$ANS" | tail -n 1)" == "t|$(sql primary "SET sift.enable_agg = off" "$ANS" | tail -n 1)"
finish
