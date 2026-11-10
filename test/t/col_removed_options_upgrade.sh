#!/usr/bin/env bash
# key_cut_rows, point_columns and order_columns, deprecated in 4.3, are removed in 4.4 and stay accepted and ignored: an index that the released 4.3.0
# library wrote with each of them stored opens in this tree, answers as the heap, takes ALTER INDEX of another option, REINDEX and REINDEX CONCURRENTLY, survives
# ALTER EXTENSION UPDATE, and the dump of the old database restores into this tree. RESET drops them.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=v4.3.0
git -C "$REPO" rev-parse -q --verify "refs/tags/$ref" >/dev/null || { echo "1..0 # SKIP no release tag $ref"; exit 0; }
newroot=$ROOT
ext=$(sed -n "s/^default_version *= *'\(.*\)'/\1/p" "$REPO/pg_sift.control")
plan 14

build_old "$ref"; oldroot=$OLDROOT
IXS="ev_kcr ev_pc ev_oc ev_all"
node_init us 43 'autovacuum = off'
use_root us "$oldroot"
node_start us || bail "cannot start $ref"
sql us "CREATE EXTENSION pg_sift" "CREATE TABLE ev (id int, ts timestamp, g int, v int)" \
	"INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 minute', i % 50, i % 1000 FROM generate_series(1, 30000) i" \
	"CREATE INDEX ev_kcr ON ev USING sift_col (id, ts, g, v) WITH (key_cut = 'g', key_cut_rows = 500)" \
	"CREATE INDEX ev_pc ON ev USING sift_col (id, ts, g, v) WITH (point_columns = 'id')" \
	"CREATE INDEX ev_oc ON ev USING sift_col (id, ts, g, v) WITH (order_columns = 'ts')" \
	"CREATE INDEX ev_all ON ev USING sift_col (id, ts, g, v) WITH (key_cut_rows = 300, point_columns = 'id', order_columns = 'ts')" \
	"VACUUM (FREEZE, ANALYZE) ev" >/dev/null 2>&1 || bail "the release cannot be set up"
stored() { sql us "SELECT string_agg(relname || '=' || array_to_string(reloptions, ','), ' ' ORDER BY relname) FROM pg_class WHERE relname IN ('ev_kcr','ev_pc','ev_oc','ev_all')"; }
before=$(stored)
check "the release $ref stored the options of the four indexes" test "$(grep -o -E 'key_cut_rows|point_columns|order_columns' <<<"$before" | sort | uniq -c | tr -s ' \n' ' ')" == " 2 key_cut_rows 2 order_columns 2 point_columns "
# the answer through one index: the others are marked invalid inside a transaction that rolls back
Q="SELECT g, count(*), sum(v) FROM ev WHERE id BETWEEN 1000 AND 9000 GROUP BY g ORDER BY g"
ans() { sql us "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "SELECT md5(string_agg(x::text, ',')) FROM ($Q) x" | tail -n 1; }
ans_ix() { # INDEX: only that index is visible to the planner
	local ix=$1 other
	{ echo "BEGIN;"; for other in $IXS; do [[ $other == "$ix" ]] || echo "UPDATE pg_index SET indisvalid = false WHERE indexrelid = '$other'::regclass;"; done
	echo "SET LOCAL max_parallel_workers_per_gather = 0; SELECT md5(string_agg(x::text, ',')) FROM ($Q) x; ROLLBACK;"; } | sql us | tail -n 1
}
want=$(ans off)
node_stop us

use_root us "$newroot"
node_start us || bail "cannot start this tree on $ref data"
check "this library on the old extension: each index answers as the heap" \
	test "$(for ix in $IXS; do ans_ix "$ix"; done | sort -u | tr '\n' ' ')" == "$want "
check "... and sift.col_verify finds nothing wrong" test -z "$(for ix in $IXS; do sql us "SELECT sift.col_verify('$ix'::regclass, true)" 2>&1; done)"
check "ALTER INDEX of another option on an index that stores the removed ones" \
	test "$(sql us "ALTER INDEX ev_all SET (cube_minmax = true)" "ALTER INDEX ev_kcr SET (cube_minmax = true)" 2>&1 | grep -c ERROR)" == 0
out=$(for ix in $IXS; do sql us "SET client_min_messages = notice" "REINDEX INDEX $ix" 2>&1; done)
check "REINDEX of each index: no error, no NOTICE, the options stay stored" \
	test "$(grep -c -E 'ERROR|removed in 4.4' <<<"$out")|$(stored | grep -o -E 'key_cut_rows|point_columns|order_columns' | wc -l)" == "0|6"
check "REINDEX INDEX CONCURRENTLY of an index that stores them" test "$(sql us "REINDEX INDEX CONCURRENTLY ev_pc" 2>&1 | grep -c ERROR)" == 0
check "each index answers as the heap after the REINDEX" test "$(for ix in $IXS; do ans_ix "$ix"; done | sort -u | tr '\n' ' ')" == "$want "
dump=$TAPDIR/dump.sql
pg_dump -h "$(dir us)/sock" -p "$(port us)" -U postgres -d postgres >"$dump"
check "the dump names the options as stored" test "$(grep -c -E 'key_cut_rows|point_columns|order_columns' "$dump")" -ge 4

sql us "ALTER EXTENSION pg_sift UPDATE" >/dev/null 2>"$TAPDIR/upd.err"
check "ALTER EXTENSION UPDATE from $ref (to $(sql us "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'"))" \
	test "$(sql us "SELECT count(*) FROM pg_extension WHERE extname = 'pg_sift' AND extversion = '$ext'")" == 1
check "after the update: the indexes answer as the heap and the writes go on" \
	test "$(sql us "INSERT INTO ev SELECT i, now(), i % 50, i % 1000 FROM generate_series(30001, 30100) i" "VACUUM ev" >/dev/null 2>&1; echo $?)|$(ans on)" == "0|$(ans off)"

sql us "CREATE DATABASE r43" >/dev/null
check "the dump of the $ref database restores without an error" \
	test "$(PGOPTIONS='-c client_min_messages=warning' psql -X -At -q -h "$(dir us)/sock" -p "$(port us)" -U postgres -d r43 -f "$dump" 2>&1 | grep -c ERROR)" == 0
check "... and the four indexes are there with their options" \
	test "$(sql us "SELECT 1" >/dev/null; psql -X -At -h "$(dir us)/sock" -p "$(port us)" -U postgres -d r43 -c "SELECT count(*) FROM pg_class WHERE relname IN ('ev_kcr','ev_pc','ev_oc','ev_all') AND reloptions IS NOT NULL")" == 4

sql us "ALTER INDEX ev_all RESET (key_cut_rows, point_columns, order_columns)" "ALTER INDEX ev_kcr RESET (key_cut_rows)" "ALTER INDEX ev_pc RESET (point_columns)" "ALTER INDEX ev_oc RESET (order_columns)" >/dev/null 2>&1
check "RESET drops them" test "$(stored | grep -c -E 'key_cut_rows|point_columns|order_columns')" == 0
check "a CREATE INDEX that names one is accepted and says so" \
	test "$(sql us "SET client_min_messages = notice" "CREATE INDEX ev_new ON ev USING sift_col (id, ts) WITH (point_columns = 'id')" 2>&1 | grep -c 'was removed in 4.4 and is ignored')" == 1
finish
