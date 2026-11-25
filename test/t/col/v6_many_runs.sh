#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# A partition of a Format 2 index that holds more parts than 64 is read whole by a source of the column engine that merges its parts by the key of the index (a window function): every part is a
# run of the merge, and the runs past the 64th used to be left out without a word, so the window lost the rows of the newest parts.
. "$(dirname "$0")/lib.bash"

plan 3
[[ -n ${SIFT_TESTING:-} ]] || { skip 3 "needs the part listing of the testing flavour"; finish; }
BATCHES=70
node_init mr 98 "autovacuum = off
sift.maintenance_naptime = 60000
sift.test_create_format1 = off"
node_start mr || bail "start failed"
Q="SELECT count(*) AS n, sum(s) AS total, max(s) AS top FROM (SELECT sum(v) OVER (PARTITION BY g ORDER BY a) AS s FROM w) x"
MODE_index="SET max_parallel_workers_per_gather = 0; SET sift.agg_force = on; SET sift.enable_agg = on"
MODE_heap="SET max_parallel_workers_per_gather = 0; SET sift.enable_agg = off"
sql mr "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
	"CREATE TABLE w (g int, a int, b bigint, v int) WITH (autovacuum_enabled = false)" \
	"CREATE INDEX w_idx ON w USING sift_col (g, a, b, v) WITH (key_order = 'g, a')" >/dev/null || bail "setup failed"
for ((b = 0; b < BATCHES; b++)); do
	sql mr "INSERT INTO w SELECT 0, $b * 2100 + i, i % 10, i % 100 FROM generate_series(1, 2100) i" >/dev/null || bail "insert $b failed"
done
sql mr "ANALYZE w" >/dev/null || bail "analyze failed"

PARTS=$(sql mr "SELECT count(*) FROM sift._test_f2_parts('w_idx', 1, NULL)")
check "one partition holds more parts than 64 ($PARTS)" test "$PARTS" -gt 64
out=$(sql mr "${MODE_index}" "EXPLAIN (COSTS OFF) $Q")
check "the window is read from the index" test "$(grep -c 'Custom Scan (SiftAggV6Window)' <<<"$out")" == 1
ON=$(sql mr "${MODE_index}" "$Q")
OFF=$(sql mr "${MODE_heap}" "$Q")
diag "index: $ON"
diag "heap:  $OFF"
check "the window is the heap's, no part left out" test "$ON" == "$OFF"
finish
