#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# Column deltas across a numeric ALTER TABLE .. TYPE that rewrites nothing, and the cost of reading a chain. A numeric(10,2) column keeps its parts as scaled integers after it becomes numeric:
# the patches written before (scaled integers) and after (numerics) the ALTER are read over those parts by the plain scan, the aggregate scan and the fold of VACUUM, as the heap has the rows.
# A fold reads the chain once for the whole compaction (the records of 50 statements spread over 200 granules), not once for each granule; the ordered scan of a sift_col index that merges
# parts builds the column deltas of each part once, not each time the merge goes back to it.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 12
CONF="autovacuum = off
max_parallel_workers_per_gather = 0
sift.async_maintenance = off
sift.maintenance_naptime = 60000"
[[ -z ${SIFT_TESTING:-} ]] || CONF+=$'\nsift.test_create_format1 = off'
node_init cf 86 "$CONF"
node_start cf || bail "start failed"
sql cf "CREATE EXTENSION pg_sift" >/dev/null || bail "setup failed"
# used REL: the blocks REL uses (what an UPDATE that keeps its rows in their parts adds is the pages of its column deltas)
used() { sql cf "SELECT used_blocks FROM sift.space('$1')"; }
# blocks NODE REL KIND STATEMENT: the blocks of REL (KIND tables or indexes) the statement reads
blocks() {
	local stat=$([[ $3 == indexes ]] && echo "idx_blks_hit + idx_blks_read" || echo "heap_blks_hit + heap_blks_read") col=$([[ $3 == indexes ]] && echo indexrelname || echo relname)
	sql "$1" "SELECT pg_stat_force_next_flush()" "SELECT $stat FROM pg_statio_user_$3 WHERE $col = '$2'" "$4" "SELECT pg_stat_force_next_flush()" "SELECT pg_stat_clear_snapshot()" \
		"SELECT $stat FROM pg_statio_user_$3 WHERE $col = '$2'" | grep -E '^[0-9]+$' | sed -n '1p;$p' | paste -sd' ' - | awk '{ print $2 - $1 }'
}

# 1. numeric(10,2) -> numeric with patches before and after the ALTER, a twin heap table taking the same statements
declare -A RC=()
for t in w wh; do
	am=$([[ $t == w ]] && echo sift_table || echo heap)
	sql cf "CREATE TABLE $t (id bigint, p numeric(10,2), pad text) USING $am" "INSERT INTO $t SELECT g, g + 0.25, 'x' || g % 7 FROM generate_series(1, 200000) g" \
		"VACUUM (FREEZE, ANALYZE) $t" >/dev/null || bail "setup of $t failed"
	[[ $t == wh ]] || w0=$(used w)
	sql cf "UPDATE $t SET p = p + 1 WHERE id % 1000 = 0" "ALTER TABLE $t ALTER COLUMN p TYPE numeric" \
		"UPDATE $t SET p = p + 0.001 WHERE id % 1000 = 1" "UPDATE $t SET p = 'NaN' WHERE id = 7" "UPDATE $t SET p = 'Infinity' WHERE id = 8" "ANALYZE $t" >/dev/null
	RC[$t]=$?
done
check "the UPDATEs around the ALTER run on the sift_table as on the heap" test "${RC[w]}" == 0 -a "${RC[wh]}" == 0
rows() { sql cf "SET sift.enable_agg = $2" "SELECT md5(string_agg(id || ':' || p, ',' ORDER BY id)) FROM $1"; }
sums() { sql cf "SET sift.enable_agg = $2" "SELECT sum(p) FILTER (WHERE p < 'Infinity'), count(*), min(p), max(p) FILTER (WHERE p < 'Infinity'), count(*) FILTER (WHERE p = 'NaN') FROM $1"; }
check "the UPDATEs keep the rows in their parts, a chain of patches of both kinds" test "$(used w)" -gt "$w0"
check "the rows of the plain scan are the heap's" test "$(rows w off)" == "$(rows wh off)"
out=$(sql cf "EXPLAIN (COSTS OFF) SELECT sum(p) FROM w")
check "the aggregate scan reads the column" grep -q SiftAgg <<<"$out"
check "the sums of the aggregate scan are the heap's" test "$(sums w on)" == "$(sums wh off)"
sql cf "VACUUM w" >/dev/null
check "after VACUUM folds the patches the rows are the heap's" test "$(rows w on)" == "$(rows wh off)" -a "$(sums w on)" == "$(sums wh off)"
check "sift.check finds nothing" test "$(sql cf "SELECT count(*) FROM sift.check('w'::regclass)")" == 0

# 2. 200 granules, the records of 50 statements each on every granule: the fold reads the chain once, not once for each granule
sql cf "CREATE TABLE f (id int, b int, e int) USING sift_table" "INSERT INTO f SELECT i, i, i FROM generate_series(1, 1640000) i" "SELECT busy FROM sift.table_maintain('f')" >/dev/null ||
	bail "setup of f failed"
f0=$(used f)
UPD=()
for k in $(seq 0 49); do UPD+=("UPDATE f SET b = b + 1, e = -e WHERE id % 6553 = $k"); done
sql cf "${UPD[@]}" >/dev/null || bail "the updates of f failed"
granules=$(sql cf "SELECT sum(granules) FROM sift.parts('f')")
pages=$(($(used f) - f0))
vac=$(blocks cf f tables "VACUUM f")
diag "granules=$granules delta pages=$pages blocks read by VACUUM=$vac"
check "the chain spans every granule" test "$granules" -ge 200 -a "$pages" -ge 40
check "VACUUM reads the chain once, not once a granule" test "$vac" -lt $((granules * pages / 2))
check "the folded rows are the updated ones" test "$(sql cf "SELECT sum(b::bigint), sum(e::bigint) FROM f")" == \
	"$(sql cf "SELECT sum(i + (i % 6553 < 50)::int), sum(CASE WHEN i % 6553 < 50 THEN -i ELSE i END) FROM generate_series(1, 1640000) i")"

# 3. an ordered scan of a sift_col index merging 8 parts, each with column deltas, whose keys alternate
INS=()
for b in $(seq 0 7); do INS+=("INSERT INTO c SELECT i * 8 + $b, i, i % 7 FROM generate_series(0, 9999) i"); done
sql cf "CREATE TABLE c (id bigint, amount bigint, other int) WITH (autovacuum_enabled = false)" "CREATE INDEX c_other ON c (other)" \
	"CREATE INDEX c_idx ON c USING sift_col (id, amount) WITH (key_order = 'id, amount')" "${INS[@]}" >/dev/null || bail "setup of c failed"
c0=$(used c_idx)
sql cf "UPDATE c SET other = other + 1 WHERE id % 3 = 0" >/dev/null || bail "the update of c failed"
parts=$(sql cf "SELECT count(*) FROM sift.parts('c_idx')")
pages=$(($(used c_idx) - c0))
Q="SELECT count(*), sum(id), sum(amount) FROM (SELECT id, amount FROM c ORDER BY id) s"
ORD="SET enable_seqscan = off; SET enable_bitmapscan = off; SET enable_sort = off; SET sift.enable_agg = off"
ord=$(blocks cf c_idx indexes "$ORD; $Q")
diag "parts=$parts delta pages=$pages index blocks of the ordered scan=$ord"
check "the ordered scan merges the parts and gives every row" test "$(sql cf "$ORD; EXPLAIN (COSTS OFF) $Q" | grep -c 'Index Only Scan using c_idx')" == 1 -a \
	"$(sql cf "$ORD; $Q")" == "80000|$((79999 * 80000 / 2))|$((8 * 9999 * 10000 / 2))"
check "the ordered scan builds the deltas of each part once" test "$parts" -ge 8 -a "$ord" -lt $((parts * pages))
finish
