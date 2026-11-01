#!/usr/bin/env bash
# An index the real 3.7.1 library wrote, with a global order (order_array_columns, column format 38), is read by this library as before: it verifies, its ordered
# reads (ASC and DESC, an OFFSET, a range) return the heap's, and it takes writes. order_layout = 'compact' then writes the compact order (column format 41) for the segments
# a merge or a REINDEX writes from then on, next to the old whole ones in one index; the ordered reads stay the heap's over both layouts.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots; none supports 19 yet)'; exit 0; fi
ref=v3.7.1
if ! git -C "$REPO" rev-parse --verify -q "$ref^{commit}" >/dev/null; then
	echo "1..0 # SKIP released library source $ref is unavailable"
	exit 0
fi
newroot=$ROOT
build_old "$ref"; oldroot=$OLDROOT

node_init legacy 2 'autovacuum = off
sift.async_maintenance = off'
use_root legacy "$oldroot"
node_start legacy || bail "cannot start $ref"
sql legacy "CREATE EXTENSION pg_sift" "SELECT setseed(0.42)" "CREATE TABLE orders (id int, ts timestamp, qty bigint)" \
	"INSERT INTO orders SELECT i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', i FROM generate_series(1, 30000) i" \
	"CREATE INDEX o_col ON orders USING sift_col (id, ts, qty) WITH (write_buffer_limit = 4096, order_columns = 'ts', order_array_columns = 'ts')" \
	"INSERT INTO orders SELECT i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', i FROM generate_series(30001, 34000) i" \
	"SELECT sift.col_promote('o_col')" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) orders" \
	"CREATE TABLE names (id int, name text)" "INSERT INTO names SELECT i, md5(i::text) FROM generate_series(1, 20000) i" \
	"CREATE INDEX n_col ON names USING sift_col (id, name) WITH (write_buffer_limit = 4096, order_array_columns = 'name')" \
	"INSERT INTO names SELECT i, md5(i::text) FROM generate_series(20001, 23000) i" "SELECT sift.col_promote('n_col')" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) names" >/dev/null || bail "$ref setup"
FORMATS="SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) FROM sift.col_segments('o_col') WHERE kind = 'immutable'"
ORD="SELECT md5(string_agg(k::text, ',')) FROM (SELECT ts AS k FROM orders ORDER BY ts LIMIT 40 OFFSET 900) s"
ORDD="SELECT md5(string_agg(k::text, ',')) FROM (SELECT ts AS k FROM orders WHERE ts <= TIMESTAMP '2026-08-01' ORDER BY ts DESC LIMIT 100 OFFSET 2000) s"
ORDN="SELECT md5(string_agg(k, ',')) FROM (SELECT name AS k FROM names ORDER BY name LIMIT 40 OFFSET 900) s"
ORDND="SELECT md5(string_agg(k, ',')) FROM (SELECT name AS k FROM names WHERE name >= '3' AND name < '6' ORDER BY name DESC LIMIT 100 OFFSET 500) s"
FORMATN="SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) FROM sift.col_segments('n_col') WHERE kind = 'immutable'"
wantn=$(sql legacy "SET sift.enable_agg = off" "$ORDN")
wantnd=$(sql legacy "SET sift.enable_agg = off" "$ORDND")
want=$(sql legacy "SET sift.enable_agg = off" "$ORD")
wantd=$(sql legacy "SET sift.enable_agg = off" "$ORDD")
check "the 3.7.1 library wrote the order arrays as column format 38 or below" test "$(sql legacy "SELECT max(format) <= 38 FROM sift.col_segments('o_col') WHERE kind = 'immutable'")" == t
check "and answers the ordered reads as the heap does" test "$(sql legacy "$ORD")|$(sql legacy "$ORDD")" == "$want|$wantd"
check "and so does its text order array (the strings in the block)" test "$(sql legacy "$ORDN")|$(sql legacy "$ORDND")|$(sql legacy "SELECT max(format) <= 38 FROM sift.col_segments('n_col') WHERE kind = 'immutable'")" == "$wantn|$wantnd|t"
node_stop legacy

use_root legacy "$newroot"
node_start legacy || bail "cannot start the new library"
sql legacy "ALTER EXTENSION pg_sift UPDATE" >/dev/null || bail "SQL update"
verify() { test -z "$(sql legacy "SELECT sift.col_verify('o_col', true)" 2>&1)"; }
check "the new library verifies the index of 3.7.1 and its ordered reads are the heap's" test "$(verify && echo ok)|$(sql legacy "$ORD")|$(sql legacy "$ORDD")" == "ok|$want|$wantd"
sql legacy "INSERT INTO orders SELECT i, TIMESTAMP '2026-01-01' + random() * 300 * INTERVAL '1 day', i FROM generate_series(34001, 36000) i" "SELECT sift.col_promote('o_col')" >/dev/null
check "it takes writes: the new segment is promoted next to the old ones and the reads are still the heap's" \
	test "$(verify && echo ok)|$(sql legacy "SET sift.enable_agg = off" "$ORD")" == "ok|$(sql legacy "$ORD")"

# the text order array 3.7.1 wrote (the strings in the block) is read as before; the segments written from now on name their strings by dictionary code (column format 42)
verifyn() { test -z "$(sql legacy "SELECT sift.col_verify('n_col', true)" 2>&1)"; }
check "the new library verifies the text order array of 3.7.1 and its ordered reads are the heap's" test "$(verifyn && echo ok)|$(sql legacy "$ORDN")|$(sql legacy "$ORDND")" == "ok|$wantn|$wantnd"
sql legacy "INSERT INTO names SELECT i, md5(i::text) FROM generate_series(23001, 26000) i" "SELECT sift.col_promote('n_col')" >/dev/null
check "a new segment next to it names its strings by code (column format 42) and the reads are the heap's" test "$(sql legacy "$FORMATN" | grep -c 42)|$(verifyn && echo ok)|$(sql legacy "SET sift.enable_agg = off" "$ORDN")" == "1|ok|$(sql legacy "$ORDN")"
sql legacy "SELECT sift.col_merge('n_col', true)" >/dev/null
check "a merge writes only the codes (column format 42) and the reads are still the heap's" test "$(sql legacy "$FORMATN")|$(verifyn && echo ok)|$(sql legacy "SET sift.enable_agg = off" "$ORDN")|$(sql legacy "SET sift.enable_agg = off" "$ORDND")" == "42|ok|$(sql legacy "$ORDN")|$(sql legacy "$ORDND")"

# the option and a merge: the old whole order and the new compact one in one index, then only compact
sql legacy "ALTER INDEX o_col SET (order_layout = 'compact')" >/dev/null
if [[ $(sql legacy "SELECT count(*) FROM pg_settings WHERE name = 'sift.test_col_gorder_ends'") == 1 ]]; then
	sql legacy "ALTER SYSTEM SET sift.test_col_gorder_ends = 64" "SELECT pg_reload_conf()" >/dev/null
	sleep 1
	sql legacy "SELECT sift.col_merge('o_col', true)" >/dev/null
	check "a merge with order_layout = compact writes column format 41" test "$(sql legacy "$FORMATS")" == 41
	check "the merged index verifies and the ordered reads are the heap's" test "$(verify && echo ok)|$(sql legacy "SET sift.enable_agg = off" "$ORD")|$(sql legacy "SET sift.enable_agg = off" "$ORDD")" == "ok|$(sql legacy "$ORD")|$(sql legacy "$ORDD")"
	sql legacy "ALTER INDEX o_col SET (order_layout = 'whole')" "REINDEX INDEX o_col" >/dev/null
	check "order_layout = whole and a REINDEX write the order that 3.7.1 reads (column format 38)" test "$(sql legacy "$FORMATS")" == 38
	sql legacy "ALTER INDEX o_col SET (order_layout = 'compact')" "REINDEX INDEX o_col" >/dev/null
	check "order_layout = compact and a REINDEX write column format 41" test "$(sql legacy "$FORMATS")" == 41
	check "and the reads are the heap's" test "$(verify && echo ok)|$(sql legacy "SET sift.enable_agg = off" "$ORD")" == "ok|$(sql legacy "$ORD")"
else skip 5 "a release build has no test settings (the compact order needs sift.test_col_gorder_ends to be written for a small table)"; fi
node_stop legacy
echo "1..$NTEST"
finish
