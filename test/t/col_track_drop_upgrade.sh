#!/usr/bin/env bash
# The update that lets DROP INDEX CONCURRENTLY drop a tracked sift_col index (4.2.1): a database that an earlier release wrote has its two track_changes triggers depending on the index, which
# that command cannot drop. After the library is replaced the triggers of such an index are not found until ALTER EXTENSION UPDATE (the reads check the heap, right and slower); the update
# makes the index depend on its triggers (sift.col_track_convert), and then the drops work, with writes on the table before and after, and no trigger is left that no index depends on.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if ((PG_MAJOR != 18)); then echo '1..0 # SKIP the released libraries build on PostgreSQL 18 only (two library roots)'; exit 0; fi
ref=$(git -C "$REPO" tag -l 'v4.[12].*' --sort=-v:refname | head -n 1)
[[ -n $ref ]] || { echo '1..0 # SKIP no release tag v4.1.* or v4.2.*'; exit 0; }
newroot=$ROOT
plan 11

build_old "$ref"; oldroot=$OLDROOT
node_init us 42 'autovacuum = off'
use_root us "$oldroot"
node_start us || bail "cannot start $ref"
sql us "CREATE EXTENSION pg_sift" "CREATE TABLE h (id int PRIMARY KEY, g int NOT NULL, v int NOT NULL)" \
	"INSERT INTO h SELECT i, i % 10, i FROM generate_series(1, 4000) i" "CREATE INDEX h_c ON h USING sift_col (id, g, v)" "CREATE INDEX h_d ON h USING sift_col (id, g, v)" \
	"VACUUM (FREEZE, ANALYZE) h" >/dev/null 2>&1 || bail "the release cannot be set up"
Q="SELECT string_agg(g || ':' || n || ':' || s, ' ' ORDER BY g) FROM (SELECT g, count(*) n, sum(v) s FROM h GROUP BY g) q"
ans() { sql us "SET max_parallel_workers_per_gather = 0" "SET sift.enable_agg = $1" "$Q" 2>/dev/null | tail -n 1; }
# the triggers an index of the table depends on, the triggers the indexes are the dependents of, and those no index owns
fwd() { sql us "SELECT count(*) FROM pg_depend d WHERE d.classid = 'pg_trigger'::regclass AND d.refclassid = 'pg_class'::regclass AND d.refobjid IN (SELECT indexrelid FROM pg_index WHERE indrelid = 'h'::regclass)" 2>/dev/null; }
rev() { sql us "SELECT count(*) FROM pg_depend d WHERE d.refclassid = 'pg_trigger'::regclass AND d.classid = 'pg_class'::regclass AND d.objid IN (SELECT indexrelid FROM pg_index WHERE indrelid = 'h'::regclass)" 2>/dev/null; }
orphans() { sql us "SELECT count(*) FROM pg_trigger g WHERE g.tgname LIKE 'sift_track_%' AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.objid = g.oid AND d.classid = 'pg_trigger'::regclass AND d.refclassid = 'pg_class'::regclass) AND NOT EXISTS (SELECT 1 FROM pg_depend d WHERE d.refobjid = g.oid AND d.refclassid = 'pg_trigger'::regclass)" 2>/dev/null; }
writes() { sql us "UPDATE h SET v = v + 1 WHERE id <= 10" "DELETE FROM h WHERE id = (SELECT max(id) FROM h)" "INSERT INTO h SELECT max(id) + 1, 1, 1 FROM h" >/dev/null 2>&1; }
check "the release $ref made the triggers the dependents of its two indexes: 4 of them" test "$(fwd)|$(rev)" == "4|0"
node_stop us

use_root us "$newroot"
node_start us || bail "cannot start this tree on $ref data"
check "this library on the old extension: writes work, the reads answer as the heap" test "$(writes; echo $?)|$(ans on)" == "0|$(ans off)"
check "... and DROP INDEX CONCURRENTLY of the old index still has its old refusal until the update" test "$(sql us "DROP INDEX CONCURRENTLY h_d" 2>&1 | grep -c 'must be first action')" == 1

sql us "ALTER EXTENSION pg_sift UPDATE" >/dev/null 2>"$TAPDIR/upd.err"
check "ALTER EXTENSION UPDATE from $ref (to $(sql us "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'"))" test "$(sql us "SELECT count(*) FROM pg_extension WHERE extname = 'pg_sift' AND extversion = '4.2.1'")" == 1
check "the index depends on the triggers now, and no trigger depends on an index" test "$(fwd)|$(rev)|$(orphans)" == "0|4|0"
check "sift.col_track_convert() has nothing left to convert" test "$(sql us "SELECT sift.col_track_convert()")" == 0
check "writes work after the update, and the reads answer as the heap" test "$(writes; echo $?)|$(ans on)" == "0|$(ans off)"
sql us "REINDEX INDEX h_c" >/dev/null 2>&1
check "REINDEX makes the log complete again: the triggers are found, the heap-free read works" test "$(sql us "SELECT triggers, complete, heap_free FROM sift.col_tracking('h_c')" | tail -n 1)" == "t|t|t"

sql us "DROP INDEX CONCURRENTLY h_d" >/dev/null 2>"$TAPDIR/drop.err"
check "DROP INDEX CONCURRENTLY of an index of $ref: dropped, the triggers of the other index stay, none is orphaned" test "$(sql us "SELECT count(*) FROM pg_class WHERE relname = 'h_d'" 2>/dev/null)|$(sql us "SELECT count(*) FROM pg_trigger WHERE tgrelid = 'h'::regclass AND tgname LIKE 'sift_track_%'")|$(orphans)" == "0|2|0"
check "... and the writes of the table go on" test "$(writes; echo $?)" == 0
sql us "DROP INDEX CONCURRENTLY h_c" >/dev/null 2>&1
check "the last one: no index, no trigger" test "$(sql us "SELECT count(*) FROM pg_trigger WHERE tgname LIKE 'sift_track_%'")|$(orphans)" == "0|0"
finish
