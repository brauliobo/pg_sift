#!/usr/bin/env bash
# The memory trackers of the main paths (core/memtrack.h): at the scale 1 every tracker of a search index passes and is recorded by
# sift._test_mem_highwater(); at sift.test_mem_budget_scale 0.0001 each class (build, merge, search, verify) fails with the tracker, the actual number and the budget.
# The release build has no scope: the test needs the test build.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 23
node_init mb 61 "autovacuum = off
sift.test_create_format1 = on
max_parallel_workers_per_gather = 0
sift.async_maintenance = off"
node_start mb || bail "start failed"
sql mb "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
sql mb "CREATE TABLE d AS SELECT g AS id, 'wireless charger model m' || g % 500 || ' w' || g AS body FROM generate_series(1, 20000) g" \
	"CREATE INDEX d_idx ON d USING sift (body) WITH (build_segments = 6)" >/dev/null || bail "table failed"

out=$(sql mb "SELECT count(*) FROM d WHERE body @@> 'wireless charger'" "SELECT sift.merge('d_idx', true) >= 0" \
	"SELECT scope FROM sift._test_mem_highwater() WHERE checks > 0 ORDER BY 1" 2>&1)
check "the search and the merge run inside their budgets at scale 1" grep -q "^bm25_search$" <<<"$out"
check "sift._test_mem_highwater names the merge scope" grep -q "^bm25_merge$" <<<"$out"

red() {
	out=$(sql mb "SET sift.test_mem_budget_scale = 0.0001" "$@" 2>&1) && return 1
	grep -qE "sift memory budget exceeded: $SCOPE: [0-9]+ > [0-9]+ bytes" <<<"$out"
}
SCOPE=bm25_build; check "a build over its lowered budget fails naming bm25_build" red "CREATE INDEX d_red ON d USING sift (body)"
sql mb "DROP INDEX IF EXISTS d_red" >/dev/null
SCOPE=bm25_search; check "a search over its lowered budget fails naming bm25_search" red "SELECT count(*) FROM d WHERE body @@> 'wireless charger'"
sql mb "CREATE INDEX d_m ON d USING sift (body) WITH (build_segments = 6)" >/dev/null
SCOPE=bm25_merge; check "a merge over its lowered budget fails naming bm25_merge" red "SELECT sift.merge('d_m', true)"
SCOPE=bm25_verify; check "a verify over its lowered budget fails naming bm25_verify" red "SELECT sift.verify('d_idx', true)"
sql mb "CREATE INDEX d_many ON d USING sift (body) WITH (build_segments = 60)" >/dev/null
out=$(sql mb "SELECT sift.verify('d_many', true) IS NOT NULL" "SELECT peak FROM sift._test_mem_highwater() WHERE scope = 'bm25_verify'" | tail -1)
check "a verify of 60 segments holds under 4 MB, flat in the segments (115 MB before the per-segment contexts)" test "$out" -lt 4194304
out=$(sql mb "SELECT count(*) FROM d WHERE body @@> 'wireless charger'" "SELECT peak FROM sift._test_mem_highwater() WHERE scope = 'bm25_search'" | tail -1)
check "a search over 60 open segments holds under its 32 MB term" test "$out" -lt 33554432
sql mb "CREATE TABLE c AS SELECT g AS id, g % 7 AS k, g % 500 AS k2, 'n' || g % 300 AS txt, g::numeric AS amount FROM generate_series(1, 20000) g" \
	"CREATE INDEX c_col ON c USING sift_col (id, k, k2, txt, amount)" >/dev/null || bail "column table failed"
SCOPE=agg_group; check "an aggregate scan over its lowered budget fails naming agg_group" red "SELECT k2, count(*), sum(amount) FROM c GROUP BY 1"
SCOPE=agg_group_nospill; check "a scan that cannot spill over its lowered budget fails naming agg_group_nospill" red "SELECT count(*), sum(amount) FROM c"
sql mb "CREATE TABLE wb (id int, v text) USING sift_table" "SELECT sift.table_set_option('wb', 'inline_promote', 'off')" \
	"INSERT INTO wb SELECT g, repeat('x', 1000) || g FROM generate_series(1, 40000) g" >/dev/null || bail "write buffer table failed"
agg_peak() { sql mb "SET work_mem = '1MB'" "$1" "SELECT peak FROM sift._test_mem_highwater() WHERE scope = 'agg_group'" | tail -1; }
check "an aggregate over 40 MB of unpromoted rows holds under 16 MB, flat in the rows (50 MB before one superblock at a time)" \
	test "$(agg_peak "SELECT count(*) FROM wb")" -lt 16777216
check "an expression over the text of 40 MB of unpromoted rows holds under 16 MB (96 MB before the cache of values was bounded)" \
	test "$(agg_peak "SELECT count(*) FROM wb WHERE v <> repeat('x', 1000) || id")" -lt 16777216
check "a scan that can spill holds twice hash memory and a constant at work_mem 1MB: under 20 MB" \
	test "$(agg_peak "SELECT k2, count(*), sum(amount) FROM c GROUP BY 1")" -lt 20971520
sql mb "CREATE TABLE ww AS SELECT g AS id, repeat(md5((g / 10)::text), 128) AS k, 's' || g % 10 AS s FROM generate_series(1, 30000) g" \
	"CREATE INDEX ww_col ON ww USING sift_col (id, k, s)" "VACUUM (FREEZE, ANALYZE) ww" >/dev/null || bail "wide key table failed"
check "groups of 4 kB keys after a spill hold under 1.5 times hash memory at work_mem 1MB: under 3 MB (5 MB while the local table took 1,536 of them)" \
	test "$(agg_peak "SELECT sum(length(m)) FROM (SELECT k, max(s) m FROM ww GROUP BY 1) x")" -lt 3145728
sql mb "CREATE TABLE ww16 AS SELECT g AS id, repeat(md5(g::text), 512) AS k FROM generate_series(1, 2500) g" \
	"CREATE INDEX ww16_col ON ww16 USING sift_col (id, k)" "VACUUM (FREEZE, ANALYZE) ww16" >/dev/null || bail "16 kB wide key table failed"
out=$(sql mb "SET work_mem = '4MB'" "SET sift.agg_force = on" "SELECT count(*) FROM (SELECT k, count(*) FROM ww16 GROUP BY 1) x" \
	"SELECT peak FROM sift._test_mem_highwater() WHERE scope = 'agg_group'" | tail -2 | tr '\n' ' ')
check "2,500 distinct 16 kB keys at work_mem 4MB answer 2500 and hold under 12 MB (compact table and batch previously grew past 32 MB)" \
	test "$(awk '{print $1}' <<<"$out")" == "2500" -a "$(awk '{print $2}' <<<"$out")" -lt 12582912
sql mb "CREATE TABLE wk AS SELECT g AS id, repeat(md5(g::text), 32) AS k, g % 3 AS s, g AS v FROM generate_series(1, 24000) g" \
	"CREATE INDEX wk_col ON wk USING sift_col (id, k, s, v)" "VACUUM (FREEZE, ANALYZE) wk" >/dev/null || bail "1 kB key table failed"
wide_peak() { sql mb "SET work_mem = '$1'" "SET sift.agg_force = on" "SELECT count(x) FROM ($2) x" \
	"SELECT peak FROM sift._test_mem_highwater() WHERE scope = 'agg_group'" | tail -1; }
wk_peaks() { for q in "SELECT k, count(*) FROM wk GROUP BY 1" "SELECT k, sum(v) FROM wk GROUP BY 1" "SELECT k, s, count(*) FROM wk GROUP BY 1, 2"; do wide_peak 8MB "$q"; done; }
check "groups of 1 kB keys that leave the compact table hold under 2.5 times work_mem at 8MB (29 MB while its flush counted groups, not bytes)" \
	test "$(wk_peaks | sort -n | tail -1)" -lt 20971520
sql mb "SET sift.test_create_format1 = off" "CREATE TABLE wh AS SELECT g AS id, repeat(md5(g::text), 19200) AS k, g % 3 AS s FROM generate_series(1, 2048) g" \
	"CREATE INDEX wh_col ON wh USING sift_col (id, k, s)" "VACUUM (FREEZE, ANALYZE) wh" >/dev/null || bail "600 kB key table failed"
wh_counts() { for q in "SELECT k, count(*) FROM wh GROUP BY 1" "SELECT k, count(DISTINCT s) FROM wh GROUP BY 1" "SELECT k, s, count(*) FROM wh GROUP BY 1, 2"; do
	sql mb "SET work_mem = '4GB'" "SET sift.agg_force = on" "SELECT count(x) FROM ($q) x" 2>&1 | tail -1; done | sort -u; }
check "2,048 distinct 600 kB keys of a Format 2 index at work_mem 4GB group without an allocation past 1 GB (the compact strings and the key batch doubled into one)" \
	test "$(wh_counts)" == "2048"
sql mb "DO \$\$ DECLARE n int; BEGIN FOREACH n IN ARRAY ARRAY[64, 256] LOOP
	EXECUTE format('CREATE TABLE q%s (id int, g int, d date NOT NULL) PARTITION BY RANGE (d)', n);
	FOR i IN 0..n - 1 LOOP EXECUTE format('CREATE TABLE q%s_%s PARTITION OF q%s FOR VALUES FROM (%L) TO (%L)', n, i, n, DATE '2000-01-01' + i, DATE '2000-01-02' + i); END LOOP;
	EXECUTE format('INSERT INTO q%s SELECT i, i %% 7, %L::date + i %% %s FROM generate_series(1, %s) i', n, '2000-01-01', n, n * 4);
	EXECUTE format('CREATE INDEX q%s_col ON q%s USING sift_col (id, g, d)', n, n);
END LOOP; END \$\$" >/dev/null || bail "partitioned tables failed"
part_peak() { sql mb "SET work_mem = '1MB'" "SET seq_page_cost = 10000" "SET random_page_cost = 10000" "SELECT g, count(*), sum(id) FROM $1 GROUP BY g" \
	"SELECT peak FROM sift._test_mem_highwater() WHERE scope = 'agg_group'" | tail -1; }
p64=$(part_peak q64)
p256=$(part_peak q256)
check "a scan holds 2 kB of state for each partition it reads, not its source (79 kB): 192 more partitions add under 8 kB each at work_mem 1MB" \
	test "${p256:-999999999999}" -lt $(( ${p64:-0} + 192 * 8192 ))
SCOPE=col_verify; check "a column verify over its lowered budget fails naming col_verify" red "SELECT sift.col_verify('c_col', true)"
SCOPE=index_vacuum; check "a VACUUM over its lowered budget fails naming index_vacuum" red "UPDATE c SET amount = amount + 1 WHERE k < 2" "VACUUM c"
sql mb "CREATE TABLE ct AS SELECT g AS id, g % 7 AS k, g % 500 AS k2, 'n' || g % 300 AS txt, g::numeric AS amount FROM generate_series(1, 20000) g" >/dev/null
SCOPE=col_build; check "a column build over its lowered budget fails naming the first tracker to pass it, col_build" red "CREATE INDEX ct_col ON ct USING sift_col (id, k, k2, txt, amount)"
out=$(sql mb "SET sift.test_mem_check = off" "SET sift.test_mem_budget_scale = 0.0001" "SELECT count(*) FROM d WHERE body @@> 'wireless charger'" 2>&1)
check "sift.test_mem_check = off returns before the measure" test "$out" -gt 0
check "the server log has one SIFTMEM_SCOPE line for a scope at backend exit" grep -q "SIFTMEM_SCOPE name=bm25_" "$(dir mb)/postgres.log"
finish
