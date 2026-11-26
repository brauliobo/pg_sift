#!/usr/bin/env bash
# Spec 105 WS-105-6: statements the aggregate scan, the vector expressions and the hybrid scan run return the rows of the normal plan. A narrow numeric
# holds NaN, which is no NULL in a filter or a group key; LIMIT + OFFSET near the int64 limit does not wrap; sum(CASE WHEN
# c THEN x ELSE 0 END) of rows that are all c and all NULL x is NULL; a nondeterministic collation decides text[] && and @>; EXTRACT of an infinite
# timestamp is no finite number; and a CASE does not evaluate the arm a row does not take.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 28
node_init nr 2 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start nr || bail "start failed"
sql nr "CREATE EXTENSION pg_sift" \
	"CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false)" \
	"CREATE TABLE nt (id int, a int, flag int, x numeric(10,2), ts timestamp, doc jsonb, tags text[] COLLATE ci, body text)" \
	"INSERT INTO nt SELECT i, i % 3, 1, CASE i % 10 WHEN 1 THEN 'NaN' WHEN 2 THEN -i / 4.0 WHEN 4 THEN NULL ELSE i / 4.0 END,
		CASE i % 10 WHEN 5 THEN 'infinity' WHEN 6 THEN '-infinity' ELSE timestamp '2026-01-01' + i * interval '1 day' END,
		jsonb_build_object('a', CASE WHEN i = 0 THEN 'oops' ELSE '1.5' END), ARRAY[CASE WHEN i = 7 THEN 'Apple' ELSE 'pear' || i END], 'common word'
		FROM generate_series(0, 49999) i" \
	"CREATE TABLE nf (id int, flag int, x numeric)" "INSERT INTO nf SELECT i % 2 + 1, 1, NULL FROM generate_series(1, 50000) i" \
	"CREATE INDEX nt_col ON nt USING sift_col (id, a, flag, x, ts, doc, tags) WITH (array_lengths = on, paths = 'doc.a text')" \
	"CREATE INDEX nt_text ON nt USING sift (body)" \
	"CREATE INDEX nf_col ON nf USING sift_col (id, flag, x)" "VACUUM (FREEZE, ANALYZE) nt" "VACUUM (FREEZE, ANALYZE) nf" >/dev/null || bail "setup failed"

OFF="SET sift.enable_agg = off; SET sift.enable_scan = off; SET sift.enable_col_scan = off;"
# same DESC QUERY: the rows of QUERY are those of the normal plan, and no statement errs
same() {
	local on off
	on=$(sql nr "$2" 2>&1)
	off=$(sql nr "$OFF $2" 2>&1)
	out="sift:"$'\n'"$on"$'\n'"normal plan:"$'\n'"$off"
	check "$1" test "$on" == "$off"
}

same "a NaN numeric is not NULL in a filter" "SELECT count(*), sum(id) FROM nt WHERE x IS NULL OR id = 0"
same "and its rows pass IS NOT NULL" "SELECT count(*) FROM nt WHERE x IS NOT NULL"
same "nor in COALESCE" "SELECT count(*) FROM nt WHERE COALESCE(x, 0) = 0"
same "nor in an expression of the filter" "SELECT count(*) FROM nt WHERE x + 1 IS NULL"
same "NaN is greater than every number" "SELECT id, x FROM nt WHERE x > 0 ORDER BY id LIMIT 10"
same "so a top-N above every number of a part keeps its NaN rows" "SELECT id, x FROM nt WHERE x > 12499 ORDER BY x, id LIMIT 10"
same "and NaN is not smaller than one" "SELECT count(*) FROM nt WHERE x < 0"
same "a group key of NaN is its own group" "SELECT x, count(*) FROM nt GROUP BY x ORDER BY x"
same "so is a key of a group and a distinct count" "SELECT x, count(DISTINCT a) FROM nt GROUP BY x ORDER BY x"
same "the sum and the maximum of the numbers of a numeric that holds NaN" "SELECT a, sum(x), max(x), min(x) FROM nt WHERE x::text <> 'NaN' GROUP BY a ORDER BY a"

same "LIMIT near the int64 limit with an OFFSET takes every row after it" "SELECT count(*) FROM (SELECT id FROM nt ORDER BY id LIMIT 9223372036854775807 OFFSET 2) s"
same "the rows of the groups too" "SELECT count(*) FROM (SELECT a, count(*) FROM nt GROUP BY a ORDER BY a LIMIT 9223372036854775807 OFFSET 1) s"
same "an OFFSET of the int64 limit takes no row" "SELECT count(*) FROM (SELECT id FROM nt ORDER BY id LIMIT 5 OFFSET 9223372036854775807) s"
same "of the groups neither" "SELECT a, count(*) FROM nt GROUP BY a LIMIT 9223372036854775807 OFFSET 9223372036854775807"
same "nor of a ranked search" "SELECT id FROM nt WHERE body @@> 'common' ORDER BY id LIMIT 9223372036854775807 OFFSET 9223372036854775807"

same "sum(CASE WHEN c THEN x ELSE 0 END) of c rows that are all NULL x is NULL" "SELECT sum(CASE WHEN flag > 0 THEN x ELSE 0 END) FROM nf"
same "of a group too, and 0 where a row is not c" "SELECT id, sum(CASE WHEN flag > 0 THEN x ELSE 0 END), sum(CASE WHEN id > 1 THEN x ELSE 0 END) FROM nf GROUP BY id ORDER BY id"
same "the integer sum the same" "SELECT sum(CASE WHEN flag > 0 THEN x::int8 ELSE 0 END), sum(CASE WHEN flag > 1 THEN x::int8 ELSE 0 END) FROM nf"
same "and the sum of a narrow numeric" "SELECT sum(CASE WHEN x::text = 'NaN' THEN 1 ELSE 0 END), sum(CASE WHEN flag > 0 THEN x ELSE 0 END) FROM nt WHERE id = 4"

same "text[] && under a nondeterministic collation matches case-insensitively" "SELECT id FROM nt WHERE tags && ARRAY['apple'] COLLATE ci AND body @@> 'common' ORDER BY id"
same "and @>" "SELECT id FROM nt WHERE tags @> ARRAY['APPLE'] COLLATE ci AND body @@> 'common' ORDER BY id"

same "EXTRACT(epoch) of an infinite timestamp is no finite number" "SELECT count(*) FROM nt WHERE EXTRACT(epoch FROM ts) < 10000000000000::numeric AND ts = 'infinity'"
same "EXTRACT(year) of an infinite timestamp is not the int64 limit" "SELECT count(*) FROM nt WHERE EXTRACT(year FROM ts) = 9223372036854775807"
same "nor is it once one is added" "SELECT count(*) FROM nt WHERE EXTRACT(year FROM ts) + 1 > 0"
same "an infinite year is no group of finite ones" "SELECT EXTRACT(year FROM ts) y, count(*) FROM nt GROUP BY 1 ORDER BY 1"

same "a CASE does not cast the value of a row that takes the other arm" "SELECT sum(CASE WHEN id > 0 THEN (doc->>'a')::float8 ELSE 0 END) FROM nt"
check "the server is up" test "$(sql nr "SELECT 1")" == 1
check "the server log shows no crash" test "$(grep -c -E 'TRAP|PANIC|terminated by signal' "$(dir nr)/postgres.log")" == 0

finish
