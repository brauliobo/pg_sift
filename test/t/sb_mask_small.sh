#!/usr/bin/env bash
# A superblock of fewer than 64 pages (the 8-page flavour of scripts/scaling-gate.sh, which the sanitizer jobs build with ASan): the all-visible mask of a unit comes from
# a map of 2 bytes, and the kernels that read 16 bytes for each mask word must not run past what the superblock has (sift_vis_sb_mask) nor see stray bits as pages. The
# aggregates, with their pre-aggregates and without, answer as the native plan does, on a vacuumed table and with pages that are not all-visible.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

root=$("$REPO/scripts/scaling-gate.sh" build 2>/dev/null)
rc=$?
if ((rc == 77)); then echo '1..0 # SKIP the 8-page flavour needs PostgreSQL 18'; exit 0; fi
((rc == 0)) || { echo '1..1'; echo 'not ok 1 - build of the 8-page flavour'; exit 1; }

node_init sb 32 "autovacuum = off
sift.col_auto_merge = off
extension_control_path = '$root$(pg_config --sharedir):\$system'
dynamic_library_path = '$root$(pg_config --pkglibdir):\$libdir'"
node_start sb || bail "start failed"
sql sb "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"
sql sb "CREATE TABLE t AS SELECT i AS id, i % 7 AS k, (i * 13) % 1000 AS v FROM generate_series(1, 20000) i" \
	"CREATE INDEX ON t USING sift_col (k, v)" "VACUUM (ANALYZE) t" >/dev/null || bail "table failed"

QUERIES=("SELECT k, count(*), sum(v) FROM t GROUP BY k ORDER BY k"
	"SELECT count(*), sum(v) FROM t WHERE v > 500"
	"SELECT k, count(*), min(v), max(v) FROM t WHERE id % 3 = 0 GROUP BY k ORDER BY k")
answer() { # QUERY MODE: the answer from the sift plans (sift) or the native ones (native)
	local set="SET enable_seqscan = off"
	[[ $2 == native ]] && set="SET sift.enable_agg = off"
	sql sb "$set" "SET max_parallel_workers_per_gather = 0" "$1" | tail -n +1 | grep -v '^$' | "$REPO/scripts/sha256.sh"
}
same() { local q; for q in "${QUERIES[@]}"; do [[ $(answer "$q" sift) == "$(answer "$q" native)" ]] || return 1; done; }

plan 3
check "the aggregates over 14 units of 8 pages answer as the native plan" same
sql sb "UPDATE t SET v = v + 1 WHERE id % 50 = 0" >/dev/null || bail "update failed"
check "with pages that are not all-visible" same
sql sb "VACUUM t" >/dev/null || bail "vacuum failed"
check "after the vacuum" same
