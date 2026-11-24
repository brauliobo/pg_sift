#!/usr/bin/env bash
# Keys and arguments that the scan computes from columns (length of a text, the host of an URL, extract of a timestamp, CASE over integer columns, a key plus a constant) on the compute threads
# of the backend (agg/v6/par/derive.h). Every answer is compared with the heap executor on the same table (sift.enable_agg = off): texts of every shape (empty, multi-byte, long, no slash, a host
# of "www." alone), timestamps before the epoch, groups that no K held back, a HAVING that the K best must not ignore, and the rows that the threads leave to the scan (a NULL in a column the
# statement reads, an infinite timestamp, a first key that overflows its elided keys), which raise or answer exactly as the heap does.
. "$(dirname "$0")/lib.bash"

plan 13
node_init pd 64 "autovacuum = off"
node_start pd || bail "start failed"
SET=("SET max_parallel_workers_per_gather = 3" "SET work_mem = '256MB'" "SET sift.gpu = off")
sql pd "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"
URLS="CASE i % 23 WHEN 0 THEN '' WHEN 1 THEN 'http://www.example.com/a/b?x=1' WHEN 2 THEN 'https://example.org/' WHEN 3 THEN 'http://www./x' WHEN 4 THEN 'http://host'
	WHEN 5 THEN 'ftp://files.example.net/f' WHEN 6 THEN 'https://www.日本語.jp/パス' WHEN 7 THEN 'http:///x' WHEN 8 THEN repeat('x', 40) || '😀é'
	WHEN 9 THEN 'https://www.' || repeat('h', 30 + i % 7) || '.com/' || repeat('p', i % 300) WHEN 10 THEN 'HTTP://UPPER.example/x' WHEN 11 THEN 'é' WHEN 12 THEN 'http://a.b/c'
	ELSE 'http://site' || (i % 301) || '.example.com/path/' || (i % 17) || repeat('q', i % 50) END"
sql pd "SET sift.test_create_format1 = off" \
	"CREATE TABLE pd AS SELECT i, $URLS AS u, 'r' || (i % 41) AS r,
		TIMESTAMP '1969-12-31 23:00:00' + ((i * 977) % 400000) * INTERVAL '1 second' - CASE WHEN i % 7 = 0 THEN INTERVAL '400 years' ELSE INTERVAL '0' END AS ts,
		(i % 3 - 1)::int AS a, (i % 2)::int AS b, (i % 5)::smallint AS c, (i % 4)::smallint AS e, (i % 9973 - 5000)::int AS x, (i % 100)::int AS k FROM generate_series(1, 300000) i" \
	"CREATE TABLE pm AS SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE u END AS u, r, CASE WHEN i % 4001 = 0 THEN 'infinity'::timestamp ELSE ts END AS ts, a, b, c, e, x, k FROM pd" \
	"CREATE TABLE pe AS SELECT i, u, r, ts, a, b, c, e, CASE WHEN i % 1000 = 0 THEN -2147483648 + (i % 3) ELSE x END AS x, k FROM pd" \
	"CREATE INDEX pd_col ON pd USING sift_col (u, r, ts, a, b, c, e, x, k)" "CREATE INDEX pm_col ON pm USING sift_col (u, r, ts, a, b, c, e, x, k)" \
	"CREATE INDEX pe_col ON pe USING sift_col (u, r, ts, a, b, c, e, x, k)" "VACUUM (FREEZE, ANALYZE) pd" "VACUUM (FREEZE, ANALYZE) pm" "VACUUM (FREEZE, ANALYZE) pe" >/dev/null || bail "load failed"

thr() { sql pd "${SET[@]}" "SET client_min_messages = debug1" "$@" 2>&1; }
rows() { grep -v DEBUG; }
took() { local n; n=$(sed -n 's/.*sift agg v6: \([0-9]*\) granules on [0-9]* threads, folded by the threads/\1/p' | tail -1); echo "${n:-0}"; }
heap() { sql pd "${SET[@]}" "SET sift.enable_agg = off" "$1" 2>&1; }
same() { # QUERY: the threads took at least 16 granules and the rows are the heap's
	out=$(thr "$1")
	(( $(took <<<"$out") >= 16 )) && [[ $(rows <<<"$out") == "$(heap "$1")" ]]
}
replayed() { # QUERY: the threads took none (the scan did it) and the rows are the heap's
	out=$(thr "$1")
	(( $(took <<<"$out") == 0 )) && [[ $(rows <<<"$out") == "$(heap "$1")" ]]
}
HOST="regexp_replace(u, '^https?://(?:www\.)?([^/]+)/.*\$', '\\1')"

check "the host of an URL as a key" same "SELECT $HOST AS h, count(*) AS n FROM pd GROUP BY h ORDER BY n DESC, h LIMIT 10"
check "the host with another key and a sum of lengths, none left out by a HAVING" same "SELECT $HOST AS h, k, count(*), sum(length(u)) FROM pd GROUP BY h, k HAVING count(*) > 50 ORDER BY h, k"
check "the average length of a text, ordered by it (no K the threads rank)" same "SELECT k, avg(length(u)) AS l, count(*) FROM pd GROUP BY k HAVING count(*) > 100 ORDER BY l DESC, k LIMIT 25"
check "the length of a text that has multi-byte characters, with a filter" same "SELECT c, sum(length(r)), avg(length(u)), count(*) FROM pd WHERE u <> '' GROUP BY c ORDER BY c"
check "extract of minutes of timestamps before and after the epoch, with a text key and a K" same "SELECT c, extract(minute FROM ts) AS m, r, count(*) AS n FROM pd GROUP BY c, m, r ORDER BY n DESC, c, m, r LIMIT 20"
check "extract of the hour" same "SELECT extract(hour FROM ts) AS h, k, count(*) AS n FROM pd GROUP BY h, k ORDER BY n DESC, h, k LIMIT 20"
check "CASE over two integer columns as a key" same "SELECT a, b, CASE WHEN (a = 0 AND b = 0) THEN u ELSE '' END AS s, count(*) AS n FROM pd GROUP BY a, b, s ORDER BY n DESC, a, b, s LIMIT 20"
check "five keys, one of them a CASE and two texts" same "SELECT a, b, e, CASE WHEN (a = 0 AND b = 0) THEN u ELSE '' END AS s, r, count(*) AS n FROM pd GROUP BY a, b, e, s, r ORDER BY n DESC, a, b, e, s, r LIMIT 20"
check "a key and the keys that are it plus a constant" same "SELECT x, x - 1, x - 2, count(*) AS n FROM pd GROUP BY x, x - 1, x - 2 ORDER BY n DESC, x LIMIT 15"
out=$(thr "SELECT x, x - 1, count(*) AS n FROM pe GROUP BY x, x - 1 ORDER BY n DESC, x LIMIT 15")
check "a first key that overflows an elided one raises the error of the heap" test "$(rows <<<"$out" | grep -c 'integer out of range')" == 1 -a "$(heap "SELECT x, x - 1, count(*) AS n FROM pe GROUP BY x, x - 1 ORDER BY n DESC, x LIMIT 15" | grep -c 'integer out of range')" == 1
check "a HAVING on another aggregate than the one ranked keeps the best groups that pass it" test "$(thr "SELECT k, count(*) AS n, sum(c) AS s FROM pd GROUP BY k HAVING sum(c) < 2000 ORDER BY n DESC, k LIMIT 3" | rows)" == "$(heap "SELECT k, count(*) AS n, sum(c) AS s FROM pd GROUP BY k HAVING sum(c) < 2000 ORDER BY n DESC, k LIMIT 3")"
check "NULL texts are left to the scan, which answers as the heap does" replayed "SELECT $HOST AS h, avg(length(u)), count(*) FROM pm GROUP BY h ORDER BY h"
check "an infinite timestamp is left to the scan, which answers as the heap does" replayed "SELECT extract(minute FROM ts) AS m, count(*) FROM pm GROUP BY m ORDER BY m"
finish
