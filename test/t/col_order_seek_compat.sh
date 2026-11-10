#!/usr/bin/env bash
# Seekable order chunks (column format minor 47) beside plain ones (38) in one index: the ordered reads of top-N, OFFSET, range and IN list are the heap's over both, the index
# verifies and takes writes; and a release that does not know minor 47 refuses the index at the directory, where a segment format it does not know is invalid,
# refuses to read the table with it (an error that names the format, no wrong answer), and this tree reads the index again.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=$(git -C "$REPO" tag -l 'v4.2.*' --sort=-v:refname | head -n 1)
[[ -n $ref ]] || { echo '1..0 # SKIP no release tag v4.2.*'; exit 0; }
newroot=$ROOT
plan 8

build_old "$ref"; oldroot=$OLDROOT
node_init sk 47 'autovacuum = off
sift.col_auto_merge = off
sift.async_maintenance = off
max_parallel_maintenance_workers = 0'
use_root sk "$newroot"
node_start sk || bail "cannot start this tree"
sql sk "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE au (id int, g int, ts timestamp, pad text)" \
	"INSERT INTO au SELECT i, i % 50, timestamp '2026-01-01' + (i::bigint * 7919 % 100003) * interval '1 minute', md5(i::text) FROM generate_series(1, 60000) i" \
	"CREATE INDEX au_ix ON au USING sift_col (id, g, ts, pad) WITH (key_cut = 'off', order_array_columns = 'auto: ts')" \
	"SET sift.test_limits = 'gord_seek_pct=0'" \
	"INSERT INTO au SELECT i, i % 50, timestamp '2026-03-01' + (i::bigint * 6007 % 100003) * interval '1 minute', md5(i::text) FROM generate_series(60001, 90000) i" \
	"SELECT sift.col_promote('au_ix')" >"$TAPDIR/setup.out" 2>&1 || { cat "$TAPDIR/setup.out" >&2; bail "the index cannot be made"; }
FORMATS="SELECT string_agg(DISTINCT format::text, ',' ORDER BY format::text) FROM sift.col_segments('au_ix') WHERE kind = 'immutable'"
check "the index has seekable (47) and plain (38) order chunks" test "$(sql sk "$FORMATS")" == 38,47

Q=("SELECT id, ts FROM au ORDER BY ts DESC, id LIMIT 20" "SELECT id, ts FROM au ORDER BY ts, id LIMIT 20 OFFSET 100"
	"SELECT id, ts FROM au WHERE ts BETWEEN timestamp '2026-02-10' AND timestamp '2026-03-12' ORDER BY ts, id LIMIT 20"
	"SELECT id, ts FROM au WHERE g IN (3, 5) ORDER BY ts DESC, id LIMIT 20")
same=0
for q in "${Q[@]}"; do
	a=$(sql sk "SET sift.enable_agg = on" "$q"); b=$(sql sk "SET sift.enable_agg = off" "$q")
	[[ $a == "$b" ]] && same=$((same + 1))
done
check "the ordered reads over both layouts are the heap's" test "$same" == "${#Q[@]}"
check "the ordered read walks the arrays" test "$(sql sk "SET sift.enable_agg = on" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ${Q[0]}" | grep -c 'Global Order: true')" == 1
check "the index verifies" test "$(sql sk "SELECT sift.col_verify('au_ix', true)" 2>&1 | grep -cE 'ERROR|corrupt')" == 0
node_stop sk

use_root sk "$oldroot"
node_start sk || bail "cannot start $ref"
sql sk "SELECT sift.col_merge('au_ix', true)" >"$TAPDIR/old.out" 2>&1
check "$ref refuses a merge: it names the segment format it does not read" test "$(grep -c 'segment of column format 1.47, this build reads' "$TAPDIR/old.out")" == 1
sql sk "SELECT count(*) FROM au" >"$TAPDIR/old_read.out" 2>&1
check "$ref refuses a read of the table the same way, and returns no row" \
	test "$(grep -c 'segment of column format 1.47, this build reads' "$TAPDIR/old_read.out")" == 1 -a "$(grep -cE '^[0-9]+$' "$TAPDIR/old_read.out")" == 0
node_stop sk

use_root sk "$newroot"
node_start sk || bail "cannot start this tree again"
check "this tree reads the index again, the same rows" test "$(sql sk "SET sift.enable_agg = on" "${Q[0]}")" == "$(sql sk "SET sift.enable_agg = off" "${Q[0]}")"
check "and merges both layouts into one" test "$(sql sk "SELECT sift.col_merge('au_ix', true) > 0" "SELECT sift.col_verify('au_ix', true)" 2>&1 | grep -cE 'ERROR|corrupt')" == 0
finish
