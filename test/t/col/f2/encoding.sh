#!/usr/bin/env bash
# sift_table in a database that is not UTF8 (LATIN1, SQL_ASCII): strings with bytes above 127 are stored, read, changed by a column delta, indexed, grouped and compared as a heap twin does.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 6
node_init enc 2 "shared_preload_libraries = 'pg_sift'
sift.test_create_format1 = off
statement_timeout = '30s'
autovacuum = off"
node_start enc || bail "start failed"

SAME="SELECT (SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM enc_t x) = (SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM enc_h x)"

for encoding in LATIN1 SQL_ASCII; do
	db=enc_$(tr 'A-Z' 'a-z' <<<"$encoding")
	sql enc "CREATE DATABASE $db ENCODING '$encoding' TEMPLATE template0 LOCALE_PROVIDER libc LC_COLLATE 'C' LC_CTYPE 'C'" >/dev/null || bail "database $db"
	sql enc "\\c $db" "CREATE EXTENSION pg_sift" \
		"CREATE FUNCTION high(n int) RETURNS text LANGUAGE sql IMMUTABLE AS \$\$ SELECT convert_from(decode('636166e9' || lpad(to_hex(65 + n % 26), 2, '0'), 'hex'), current_setting('server_encoding')) \$\$" \
		"CREATE TABLE enc_t (id int PRIMARY KEY, s text, k text, n int, note text) USING sift_table" "CREATE TABLE enc_h (LIKE enc_t INCLUDING ALL)" \
		"INSERT INTO enc_t SELECT g, high(g), CASE WHEN g % 3 = 0 THEN 'plain' || g ELSE high(g) END, g, 'n' || g FROM generate_series(1, 3000) g" \
		"INSERT INTO enc_h SELECT * FROM enc_t" "CREATE INDEX enc_t_k ON enc_t (k)" "CREATE INDEX enc_h_k ON enc_h (k)" "VACUUM enc_t" >/dev/null || bail "setup of $db failed"
	check "$encoding: every row is the heap's" test "$(sql enc "\\c $db" "$SAME" | tail -n 1)" == t
	sql enc "\\c $db" "UPDATE enc_t SET s = high(n + 1) WHERE id % 7 = 0" "UPDATE enc_h SET s = high(n + 1) WHERE id % 7 = 0" "DELETE FROM enc_t WHERE id % 11 = 0" \
		"DELETE FROM enc_h WHERE id % 11 = 0" "INSERT INTO enc_t VALUES (5000, high(7), high(7), 7, 'x')" "INSERT INTO enc_h VALUES (5000, high(7), high(7), 7, 'x')" >/dev/null
	check "$encoding: after a column delta, a delete and an insert every row is the heap's" test "$(sql enc "\\c $db" "$SAME" | tail -n 1)" == t
	probe="SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM (SELECT k, count(*), min(s), max(id) FROM @T WHERE k > 'a' OR k LIKE 'caf%' GROUP BY k) x"
	sift=$(sql enc "\\c $db" "SET enable_seqscan = off" "${probe//@T/enc_t}" | tail -n 1)
	heap=$(sql enc "\\c $db" "SET enable_seqscan = off" "${probe//@T/enc_h}" | tail -n 1)
	out="sift=[$sift] heap=[$heap]"
	check "$encoding: an index lookup and a grouping on the text column are the heap's" test "$sift" == "$heap" -a -n "$sift"
done
finish
