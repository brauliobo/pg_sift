#!/usr/bin/env bash
# sift.col_refresh on an index the real 3.0.1 library wrote (an index without any of the sections of 3.3): after ALTER EXTENSION UPDATE the new library
# finds its segments stale for a changed option and rewrites them. At sift.col_format_minor = 3, the lowest, the segments stay what 3.0 reads: the 3.0.1 library
# then verifies the index, answers from it as before and takes more writes. A segment of a higher minor is left as written below its setting.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR < 18)); then echo '1..0 # SKIP two library roots require PostgreSQL 18'; exit 0; fi
ref=v3.0.1
if ! git -C "$REPO" rev-parse --verify -q "$ref^{commit}" >/dev/null; then
	echo "1..0 # SKIP released library source $ref is unavailable"
	exit 0
fi
newroot=$ROOT
use_root() {
	ROOT=$1
	node_conf legacy "extension_control_path = '$ROOT$(pg_config --sharedir):\$system'
dynamic_library_path = '$ROOT$(pg_config --pkglibdir):\$libdir'"
}
oldroot=$(SIFT_OLD_REF=$ref "$REPO/scripts/old-lib-check.sh" build) || bail "cannot build $ref"

node_init legacy 2 'autovacuum = off
sift.async_maintenance = off'
use_root "$oldroot"
node_start legacy || bail "cannot start $ref"
sql legacy "CREATE EXTENSION pg_sift" "CREATE TABLE items (id int, kind int, grp text, val bigint, amt numeric(10,2), pad text)" \
	"INSERT INTO items SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), CASE WHEN g % 7 = 0 THEN NULL ELSE (g * 977) % 100000 - 50000 END,
		((g * 91) % 20000) / 100.0 - 50, repeat('p', 100) FROM generate_series(1, 20000) g" \
	"CREATE INDEX items_idx ON items USING sift_col (id, kind, grp, val, amt) WITH (write_buffer_limit = 4096)" \
	"INSERT INTO items SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), (g * 977) % 100000 - 50000, ((g * 91) % 20000) / 100.0 - 50, repeat('p', 100) FROM generate_series(20001, 25000) g" \
	"SELECT sift.col_promote('items_idx')" "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) items" >/dev/null || bail "$ref setup"
Q="SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM (SELECT kind, count(*), sum(val), min(amt), max(amt) FROM items GROUP BY kind) x"
FORMATS="SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) FROM sift.col_segments('items_idx') WHERE kind = 'immutable'"
want=$(sql legacy "$Q")
node_stop legacy
use_root "$newroot"
node_start legacy || bail "cannot start the new library"
sql legacy "ALTER EXTENSION pg_sift UPDATE TO '3.2'" >/dev/null || bail "SQL update"
check "the 3.0.1 library wrote segments of minor 3 or below" test "$(sql legacy "SELECT max(format) <= 3 FROM sift.col_segments('items_idx') WHERE kind = 'immutable'")" == t
verify() { test -z "$(sql legacy "SELECT sift.col_verify('items_idx', true)" 2>&1)"; }
check "the new library reads the 3.0.1 index: verify and the answer are the heap's" test "$(verify && echo ok)|$(sql legacy "SET sift.enable_agg = off" "$Q")" == "ok|$want"
check "its options and its segments agree: nothing is stale" test "$(sql legacy "SELECT remaining FROM sift.col_refresh('items_idx', 0)")" == 0

# at the lowest setting a changed option reaches the old segments and the minor stays 3
sql legacy "ALTER INDEX items_idx SET (preagg = 'cube(kind: val, amt; grp: val)')" >/dev/null
check "the option leaves the old segments stale" test "$(sql legacy "SELECT remaining FROM sift.col_refresh('items_idx', 0)")" -ge 1
N=$(sql legacy "SELECT remaining FROM sift.col_refresh('items_idx', 0)")
check "refreshed at sift.col_format_minor = 3, every segment is rewritten and stays minor 3 or below" \
	test "$(sql legacy "SET sift.col_format_minor = 3" "SELECT refreshed || ' ' || remaining FROM sift.col_refresh('items_idx')")|$(sql legacy "SELECT max(format) <= 3 FROM sift.col_segments('items_idx') WHERE kind = 'immutable'")" == "$N 0|t"
check "the refreshed index verifies and answers as the heap does" test "$(verify && echo ok)|$(sql legacy "SET sift.enable_agg = off" "$Q")|$(sql legacy "SET sift.enable_agg = on" "SET seq_page_cost = 10000" "$Q")" == "ok|$want|$want"
node_stop legacy

# the 3.0.1 library reads what the new one wrote
use_root "$oldroot"
node_start legacy || bail "cannot restart $ref"
check "the 3.0.1 library verifies the refreshed index" test -z "$(sql legacy "SELECT sift.col_verify('items_idx', true)" 2>&1)"
check "and answers as before, from the segments the new library wrote" test "$(sql legacy "SET sift.enable_agg = on" "SET seq_page_cost = 10000" "$Q")" == "$want"
check "and takes more writes: promoted and merged with the refreshed segments" \
	test "$(sql legacy "INSERT INTO items SELECT g, g % 6, 'g00', 1, 1, 'p' FROM generate_series(25001, 25500) g" "SELECT sift.col_promote('items_idx') >= 0" "SELECT sift.col_merge('items_idx', true) >= 0" \
		"SELECT sift.col_verify('items_idx', true) IS NOT NULL" | tr '\n' ' ')" == "t t t "
node_stop legacy

# a segment of a higher minor than the setting stays as written: under the default setting the option writes minor 7, under 3 the segment is not rewritten
use_root "$newroot"
node_start legacy || bail "cannot restart the new library"
sql legacy "ALTER INDEX items_idx SET (cube_minmax = true, preagg = 'default')" >/dev/null
check "a refresh under the default setting writes the minor the options ask for (7)" \
	test "$(sql legacy "SELECT refreshed > 0 FROM sift.col_refresh('items_idx')")|$(sql legacy "SELECT min(format) FROM sift.col_segments('items_idx') WHERE kind = 'immutable'")" == "t|7"
sql legacy "ALTER INDEX items_idx SET (cube_minmax = false)" >/dev/null
check "a refresh under sift.col_format_minor = 3 leaves that segment as written, stale for the option as it is" \
	test "$(sql legacy "SET sift.col_format_minor = 3" "SELECT refreshed || ' ' || remaining FROM sift.col_refresh('items_idx')")|$(sql legacy "SELECT min(format) FROM sift.col_segments('items_idx') WHERE kind = 'immutable'")" == "0 0|7"
check "the new library still verifies it and answers as the heap does" test "$(verify && echo ok)|$(sql legacy "SET sift.enable_agg = on" "SET seq_page_cost = 10000" "SELECT count(*) FROM items")" == "ok|25500"
node_stop legacy
echo "1..$NTEST"
finish
