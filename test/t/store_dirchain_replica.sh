#!/usr/bin/env bash
# A chained directory replays on a hot standby: edits of a directory of entries that name no storage (insertions, deletions, reversals, counts; chain pages
# written, flipped to and freed) and writes and drops of real segments on a second index, with the leaves of 3 entries and a root of 12 leaves, replay to
# the same directory and the same chain. Both nodes run with wal_consistency_checking = generic, so that the standby compares every page it replays
# with the image the primary logged.
. "$(dirname "$0")/lib.bash"

ROUNDS=$(knob SIFT_DIRCHAIN_ROUNDS 12 4)
plan $((ROUNDS * 2 + 2))
node_init primary 2 "$WAL_CONF
wal_level = replica
max_wal_senders = 4
wal_consistency_checking = 'generic'
autovacuum = off"
node_start primary || bail "primary start failed"
SHAPE=("SET sift.test_dir_flat_max = 2" "SET sift.test_dir_leaf_max = 3" "SET sift.test_dir_root_max = 12")
sql primary "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE f (v int)" "CREATE INDEX f_idx ON f USING sift_store_test (v)" \
	"CREATE TABLE s (v int)" "CREATE INDEX s_idx ON s USING sift_store_test (v)" >/dev/null || bail "setup failed"
node_clone primary standby 3 "wal_consistency_checking = 'generic'
hot_standby_feedback = on"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

LIST="SELECT coalesce(string_agg(gen || ':' || ndocs, ',' ORDER BY ord), '') FROM sift._test_dir_list('f_idx')"
SEGS="SELECT coalesce(string_agg(gen || ':' || header || ':' || size, ' ' ORDER BY gen), '') FROM sift._test_segments('s_idx')"
HOLDS="SELECT sift._test_check('s_idx') || '|' || (sift._test_dir_check('f_idx') > 0)"
RANDOM=7
for r in $(seq "$ROUNDS"); do
	stmts=()
	for _ in 1 2 3 4; do
		have=$(sql primary "SELECT count(*) FROM sift._test_dir_list('f_idx')")
		case $((RANDOM % 5)) in
			0 | 1) stmts+=("SELECT sift._test_dir_edit('f_idx', 'ins', $((RANDOM % (have + 1))), $((1 + RANDOM % 6)))") ;;
			2) ((have > 3)) && stmts+=("SELECT sift._test_dir_edit('f_idx', 'del', $((RANDOM % (have - 2))), $((1 + RANDOM % 3)))") ;;
			3) ((have > 4)) && stmts+=("SELECT sift._test_dir_edit('f_idx', 'rev', $((RANDOM % (have - 3))), 4)") ;;
			4) ((have > 0)) && stmts+=("SELECT sift._test_dir_edit('f_idx', 'set', 0, 1)") ;;
		esac
		sql primary "${SHAPE[@]}" "${stmts[-1]:-SELECT 1}" >/dev/null || bail "round $r: ${stmts[-1]:-}"
	done
	sql primary "${SHAPE[@]}" "SELECT sift._test_seg_write('s_idx', $((200 + r * 13)), $r)" \
		"SELECT sift._test_seg_drop('s_idx', (SELECT gen FROM sift._test_segments('s_idx') ORDER BY gen OFFSET $((r % 3)) LIMIT 1))" \
		"SELECT sift._test_seg_write('s_idx', $((100 + r * 7)), $((r + 100)))" >/dev/null || bail "round $r: segments"
	catchup primary standby || bail "round $r: the standby did not catch up"
	check "round $r: the standby's directory of $(sql primary "SELECT nsegs FROM sift._test_dir_info('f_idx')") entries equals the primary's" \
		test "$(sql standby "$LIST")" == "$(sql primary "$LIST")"
	out="standby $(sql standby "$SEGS")|$(sql standby "$HOLDS" 2>&1)"$'\n'"primary $(sql primary "$SEGS")|$(sql primary "$HOLDS" 2>&1)"
	check "round $r: the segments of the second index and the chain of the first hold on the standby" \
		test "$(sql standby "$SEGS")|$(sql standby "$HOLDS")" == "$(sql primary "$SEGS")|$(sql primary "$HOLDS")"
done
check "the chain of the first index is chained on both" test "$(sql standby "SELECT chained FROM sift._test_dir_info('f_idx')")|$(sql primary "SELECT chained FROM sift._test_dir_info('f_idx')")" == "t|t"
finish
