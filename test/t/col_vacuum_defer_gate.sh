#!/usr/bin/env bash
# A VACUUM leaves the moves of a loose sift_col file to a rewrite only when the automatic rewrite will start. A rewrite that is due but waits for its WAL budget
# (sift.col_auto_merge_wal_pct) is no reason to leave the file loose: the VACUUM moves the segments down and cuts the free end itself.
. "$(dirname "$0")/lib.bash"

ROWS=$(knob SIFT_DG_ROWS 200000 100000)
plan 4
node_init defer 3 "autovacuum = off
sift.test_col_net = off"
node_start defer || bail "start failed"
sql defer "CREATE EXTENSION pg_sift" \
	"CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, $ROWS) g" \
	"VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
sql defer "SET sift.async_maintenance = off" "CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad) WITH (key_cut = grp, dead_rewrite_pct = 50)" >/dev/null || bail "create failed"
for k in 1 2 3 4; do
	sql defer "SET sift.async_maintenance = off" "INSERT INTO t SELECT id + $k * 10000000, grp, amount, pad FROM t WHERE id < 10000000 AND id % 5 = $k - 1" \
		"SELECT sift.col_promote('t_idx')" >/dev/null
done
sql defer "SELECT sift.col_merge('t_idx', true)" "SELECT pg_current_xact_id()" >/dev/null
size() { sql defer "SELECT pg_relation_size('t_idx')"; }
live() { sql defer "SELECT bytes FROM sift.col_index_info('t_idx')"; }
loose=$(size)
check "the merge leaves the file over 1.6 times the live bytes" test "$loose" -gt "$(( $(live) * 16 / 10 ))"

# dead rows in many units (a VACUUM that does not cut marks them) make a rewrite due; its WAL budget, a hundred times the segments, is far off: the automatic one does not start
sql defer "UPDATE t SET amount = amount + 1 WHERE id % 20 = 1" "ALTER INDEX t_idx SET (vacuum_truncate = false)" "VACUUM t" "ALTER INDEX t_idx RESET (vacuum_truncate)" "SELECT pg_current_xact_id()" >/dev/null
SETS=("SET sift.col_auto_merge = on" "SET sift.col_auto_merge_dirty_pct = 1" "SET sift.col_auto_merge_wal_pct = 1")
check "a rewrite is due and the automatic one waits" test "$(sql defer "${SETS[@]}" "SELECT sift.col_maintenance_due('t_idx', false) IS NOT NULL, sift.col_maintenance_due('t_idx', true) IS NULL")" == "t|t"
sql defer "${SETS[@]}" "VACUUM t" >/dev/null
sql defer "SELECT pg_current_xact_id()" >/dev/null
sql defer "${SETS[@]}" "VACUUM t" >/dev/null
diag "file $loose -> $(size), live $(live)"
check "the VACUUM that a due rewrite does not follow gives the free blocks back: the file is within 1.3 times the live bytes" test "$(size)" -le "$(( $(live) * 13 / 10 ))"
check "intact" test -z "$(sql defer "SELECT sift.col_verify('t_idx', true)" 2>&1)"
finish
