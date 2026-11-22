#!/usr/bin/env bash
# tap: pooled
# What an operator reads when the file of a sift_col index stays long: sift.col_shrink_history gives shrink_blocked_by (why the last cut
# left free blocks: a snapshot that may still read them) and last_shrink (when the file was last cut), kept in the metapage.
. "$(dirname "$0")/lib.bash"

plan 5
node_init wg 67 "autovacuum = off
sift.test_col_net = off
sift.maintenance_naptime = 200
sift.log_min_duration = 0"
node_start wg || bail "start failed"
D=$(dir wg)
sql wg "CREATE EXTENSION pg_sift" "CREATE TABLE t (id int, grp int, amount numeric(10,2), pad text) WITH (autovacuum_enabled = off)" \
	"INSERT INTO t SELECT g, g % 500, g % 997, md5(g::text) || md5((g * 7)::text) FROM generate_series(1, 100000) g" "VACUUM (FREEZE, ANALYZE) t" >/dev/null || bail "setup failed"
space() { sql wg "SELECT $1 FROM sift.col_space('t_idx')"; }
shrink() { sql wg "SELECT $1 FROM sift.col_shrink_history('t_idx')"; }
sql wg "SET sift.async_maintenance = off" "CREATE INDEX t_idx ON t USING sift_col (id, grp, amount, pad)" >/dev/null || bail "create failed"
check "a new index was never cut and nothing blocks it" test "$(shrink "shrink_blocked_by IS NULL AND last_shrink IS NULL")" == t

# 1. a snapshot older than a merge holds the blocks the merge released: the VACUUM that finds them says so
for k in 1 2; do
	sql wg "SET sift.async_maintenance = off" "INSERT INTO t SELECT id + $k * 10000000, grp, amount, pad FROM t WHERE id < 10000000 AND id % 5 = $k - 1" \
		"SELECT sift.col_promote('t_idx')" >/dev/null
done
rm -f "$D/held" "$D/release"
{ echo "BEGIN ISOLATION LEVEL REPEATABLE READ;"; echo "SELECT count(*) FROM t;"; echo "\\! touch '$D/held'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql wg >/dev/null &
HOLDER=$!
until [[ -e $D/held ]]; do sleep 0.05; done
sql wg "SELECT sift.col_merge('t_idx', true)" >/dev/null
sql wg "SET sift.async_maintenance = off" "VACUUM t" >/dev/null
check "the blocks a snapshot holds are why the file stays long" test "$(shrink "shrink_blocked_by")" == snapshot
check "and they are pending" test "$(space "pending_blocks > 0")" == t

# 2. the snapshot ends: the next VACUUM cuts the file, and the reason is gone
touch "$D/release"
wait "$HOLDER"
BEFORE=$(space relation_blocks)
shortened() { sql wg "SELECT pg_current_xact_id()" >/dev/null; sql wg "SET sift.async_maintenance = off" "VACUUM t" >/dev/null; [[ $(shrink "shrink_blocked_by IS NULL AND last_shrink IS NOT NULL") == t ]]; }
check "the file is cut and last_shrink says when" wait_for shortened 30
check "and the file is shorter ($BEFORE blocks before)" test "$(space "relation_blocks < $BEFORE")" == t

finish
