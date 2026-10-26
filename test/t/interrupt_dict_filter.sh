#!/usr/bin/env bash
# A filter on a text column is evaluated once for each entry of the column's dictionary (agg/pred_in.c dict_lut) before the scan reads a row. The
# expression is the user's and can cost milliseconds an entry: the evaluation checks for interrupts, so a statement_timeout of 100 ms ends the statement
# in well under a second, not when the last of the 150,000 entries is done (levenshtein of a 192-character name against 250 characters, about 30 us each, several seconds).
. "$(dirname "$0")/lib.bash"

plan 3
node_init idf 29 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start idf || bail "start failed"
sql idf "CREATE EXTENSION pg_sift" >/dev/null || bail "pg_sift"
sql idf "CREATE EXTENSION fuzzystrmatch" >/dev/null 2>&1 || { echo "1..0 # SKIP fuzzystrmatch is not installed"; exit 0; }
sql idf "CREATE TABLE lv (id int, g int, name text)" \
	"INSERT INTO lv SELECT i, i % 10, repeat(md5(i::text), 6) FROM generate_series(1, 150000) i" \
	"CREATE INDEX lv_i ON lv USING sift_col (id, g, name)" "VACUUM (FREEZE, ANALYZE) lv" >/dev/null || bail "setup failed"

Q="SELECT g, count(*) FROM lv WHERE levenshtein(name, repeat('abcdefghij', 25)) < 100 GROUP BY g"
check "the filter is a pushed one" test "$(sql idf "EXPLAIN (COSTS OFF) $Q" | grep -c 'Filters: name')" == 1

start=$(date +%s%N)
out=$(sql idf "SET statement_timeout = 100" "$Q" 2>&1)
ms=$(( ($(date +%s%N) - start) / 1000000 ))
check "the statement timeout ends it" test "$(grep -c 'canceling statement due to statement timeout' <<<"$out")" == 1
check "within a second (took $ms ms)" test "$ms" -lt 1000

finish
