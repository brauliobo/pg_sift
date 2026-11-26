#!/usr/bin/env bash
# The predicates and projections of a sift_col index answer what the heap answers on the values at the edge of their types (Spec 105, WS-105-5): a timestamp column against a
# timestamptz constant and a date past the range of timestamps, a date column against a timestamptz at a fall-back that crosses midnight, the group keys of a projection of
# every key type (numeric, uuid, bytea, char(n)), numeric NaN in the measures and the included columns of a projection, date_trunc of +-infinity, a literal of a
# projection that a numeric(18,2) cannot hold. Every answer is the heap's, and no backend crashes.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 10
node_init r 1 "autovacuum = off
sift.test_create_format1 = off"
node_start r || bail "start failed"
q() { sql r "$@" 2>&1; }
# the rows of a statement through the index (sift on) and through the heap (sift off), as one text each
same() { [[ $(q "SELECT p_same(\$q\$$1\$q\$)") == same ]]; }
same_order() { [[ $(q "SELECT p_same_order(\$q\$$1\$q\$, \$q\$$1\$q\$)") == same ]]; }
uses() { [[ $(q "SELECT p_uses(\$q\$$1\$q\$, '$2')") == t ]]; }

sql r "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
psql -X -q -v ON_ERROR_STOP=1 -h "$(node_sock r)" -p "$(port r)" -U postgres -d "$(node_db r)" -f "$SIFT_REPO/test/common/sql/col/proj/base.sql" >/dev/null || bail "helpers failed"
q "ALTER DATABASE postgres SET TimeZone = 'UTC'" >/dev/null

# 1. temporal predicates across types: the index answers what the heap answers
q "CREATE TABLE xt (id int, ts timestamp, tstz timestamptz, d date)" "INSERT INTO xt VALUES (1, '2026-01-01 00:00:00', '2026-01-01 00:00:00+00', '2026-01-01')" \
	"CREATE INDEX xt_i ON xt USING sift_col (ts, tstz, d)" "VACUUM xt" >/dev/null
for p in "ts < TIMESTAMPTZ '2026-01-01 00:00:01+00'" "ts < DATE '500000-01-01'" "tstz < DATE '500000-01-01'"; do
	out=$(q "SELECT p_rows(\$q\$SELECT count(*) FROM xt WHERE $p\$q\$, true) || '/' || p_rows(\$q\$SELECT count(*) FROM xt WHERE $p\$q\$, false)")
	check "$p: the index counts what the heap counts (1)" test "$out" == "(1)/(1)"
done
# a fall-back at midnight (Havana, 2025-11-02 01:00 -04 -> 00:00 -05): 00:30 -04 is on the 2nd, before the midnight PostgreSQL takes for it (00:00 -05)
q "CREATE TABLE xt2 (d date)" "INSERT INTO xt2 VALUES ('2025-11-01'), ('2025-11-02'), ('2025-11-03')" "CREATE INDEX xt2_i ON xt2 USING sift_col (d)" "VACUUM xt2" >/dev/null
dst() { q "SET TimeZone = 'America/Havana'" "SELECT p_rows(\$q\$SELECT count(*) FROM xt2 WHERE d $1 TIMESTAMPTZ '2025-11-02 00:30:00-04'\$q\$, $2)"; }
out="$(dst '<' true) $(dst '>=' true) / $(dst '<' false) $(dst '>=' false)"
check "a date against a timestamptz in the fall-back hour before midnight: the index counts what the heap counts" test "$out" == "(1) (2) / (1) (2)"

# 2. the group keys of an aggregate projection of every type the index holds
q "CREATE TABLE g (id int, n numeric(10,2), u uuid, b bytea, c char(4), v int)" "CREATE INDEX g_idx ON g USING sift_col (id, n, u, b, c, v) WITH (key_order = 'id')" \
	"INSERT INTO g SELECT i, (i % 5) + 0.25, ('00000000-0000-0000-0000-00000000000' || (i % 3))::uuid, decode(lpad((i % 4)::text, 2, '0'), 'hex'), (i % 3)::text, i
	 FROM generate_series(1, 4000) i" >/dev/null
for k in n u b c; do q "SELECT sift.create_projection('g_idx', 'g_$k', 'GROUP BY $k MEASURES count(*), sum(v)')" >/dev/null; done
q "VACUUM g" >/dev/null
out=$(for k in n u b c; do q "SELECT p_uses('SELECT $k, count(*), sum(v) FROM g GROUP BY $k', 'g_$k') || p_same('SELECT $k, count(*), sum(v) FROM g GROUP BY $k')"; done | paste -sd, -)
check "GROUP BY numeric(10,2), uuid, bytea, char(4) served by their projections: the heap's groups" test "$out" == "truesame,truesame,truesame,truesame"

# 3. numeric NaN in an aggregate projection and in a column a sort projection includes; date_trunc of +-infinity
q "CREATE TABLE h (id bigint, n numeric(10,2), ts timestamp, x numeric(18,2), v int)" "CREATE INDEX h_idx ON h USING sift_col (id, n, ts, x, v) WITH (key_order = 'id')" \
	"INSERT INTO h SELECT i, CASE WHEN i % 50 = 0 THEN 'NaN' ELSE (i % 5) + 0.25 END,
	 CASE WHEN i % 70 = 0 THEN 'infinity' WHEN i % 71 = 0 THEN '-infinity' ELSE TIMESTAMP '2026-01-01' + i * interval '1 hour' END, i, i % 4 FROM generate_series(1, 20000) i" \
	"SELECT sift.create_projection('h_idx', 'h_agg', 'GROUP BY v MEASURES count(*), sum(n), max(n), min(n)')" \
	"SELECT sift.create_projection('h_idx', 'h_day', 'GROUP BY date_trunc(''day'', ts) MEASURES count(*)')" \
	"SELECT sift.create_projection('h_idx', 'h_sort', 'ORDER BY v, id INCLUDE (n)')" "VACUUM h" >/dev/null
check "sum, max, min of a numeric with NaN rows over the aggregate projection: the heap's (NaN)" same "SELECT v, count(*), sum(n), max(n), min(n) FROM h GROUP BY v"
check "a sort projection keeps the NaN of a column it includes" eval 'uses "SELECT id, n FROM h WHERE v = 0 AND id > 96 ORDER BY id LIMIT 10" h_sort && same_order "SELECT id, n FROM h WHERE v = 0 AND id > 96 ORDER BY id LIMIT 10"'
check "ts >= '2026-02-01' over the date_trunc('day', ts) projection: -infinity is its own bucket, not one past every date" \
	eval 'uses "SELECT count(*) FROM h WHERE ts >= TIMESTAMP '\''2026-02-01'\''" h_day && same "SELECT count(*) FROM h WHERE ts >= TIMESTAMP '\''2026-02-01'\''"'
# 4. a literal a numeric(18,2) cannot hold scaled
out=$(q "SELECT sift.create_projection('h_idx', 'h_lit', 'GROUP BY v MEASURES count(*) WHERE x = 100000000000000000')")
check "x = 100000000000000000 on a numeric(18,2): refused, not wrapped" grep -q 'is not a number the column holds exactly' <<<"$out"

out=$(q "SELECT 1")
check "the server is up after every statement" test "$out" == 1
finish
