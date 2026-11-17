#!/usr/bin/env bash
# The memory budgets of the main paths (core/membudget.h, test/mem-budgets.tsv): at the scale 1 every scope of a search index passes and is recorded by
# sift._test_mem_highwater(); at sift.test_mem_budget_scale 0.0001 each class (build, merge, search, verify) fails with the scope, the actual number, the budget and the formula.
# The release build has no scope: the test needs the test build.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

plan 19
node_init mb 61 "autovacuum = off
max_parallel_workers_per_gather = 0
sift.col_auto_merge = off"
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
	grep -qE "sift memory budget exceeded: $SCOPE: [0-9]+ > [0-9]+ bytes \(.+\)" <<<"$out"
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
sql mb "SET sift.table_inline_promote = off" "CREATE TABLE wb (id int, v text) USING sift_table" \
	"INSERT INTO wb SELECT g, repeat('x', 1000) || g FROM generate_series(1, 40000) g" >/dev/null || bail "write buffer table failed"
agg_peak() { sql mb "SET work_mem = '1MB'" "$1" "SELECT peak FROM sift._test_mem_highwater() WHERE scope = 'agg_group'" | tail -1; }
check "an aggregate over 40 MB of unpromoted rows holds under 16 MB, flat in the rows (50 MB before one superblock at a time)" \
	test "$(agg_peak "SELECT count(*) FROM wb")" -lt 16777216
check "an expression over the text of 40 MB of unpromoted rows holds under 16 MB (96 MB before the cache of values was bounded)" \
	test "$(agg_peak "SELECT count(*) FROM wb WHERE v <> repeat('x', 1000) || id")" -lt 16777216
check "a scan that can spill holds twice hash memory and a constant at work_mem 1MB: under 20 MB" \
	test "$(agg_peak "SELECT k2, count(*), sum(amount) FROM c GROUP BY 1")" -lt 20971520
SCOPE=col_verify; check "a column verify over its lowered budget fails naming col_verify" red "SELECT sift.col_verify('c_col', true)"
SCOPE=index_vacuum; check "a VACUUM over its lowered budget fails naming index_vacuum" red "UPDATE c SET amount = amount + 1 WHERE k < 2" "VACUUM c"
sql mb "CREATE TABLE ct AS SELECT g AS id, g % 7 AS k, g % 500 AS k2, 'n' || g % 300 AS txt, g::numeric AS amount FROM generate_series(1, 20000) g" >/dev/null
SCOPE=col_build; check "a column build over its lowered budget fails naming col_build" red "CREATE INDEX ct_col ON ct USING sift_col (id, k, k2, txt, amount)"
check "the budget table, the generated header and the docs agree" bash -c "cd '$REPO' && python3 scripts/gen-membudget.py --check"
check "the open rows are listed for the CI report" bash -c "cd '$REPO' && python3 scripts/gen-membudget.py --open >/dev/null"
out=$(sql mb "SET sift.test_mem_check = off" "SET sift.test_mem_budget_scale = 0.0001" "SELECT count(*) FROM d WHERE body @@> 'wireless charger'" 2>&1)
check "sift.test_mem_check = off returns before the measure" test "$out" -gt 0
check "the server log has one SIFTMEM_SCOPE line for a scope at backend exit" grep -q "SIFTMEM_SCOPE name=bm25_" "$(dir mb)/postgres.log"
finish
