#!/usr/bin/env bash
# A chained directory replays on a hot standby: edits of a directory of entries that name no storage (insertions, deletions, reversals, counts; chain pages
# written, flipped to and freed) and writes and drops of real segments on a second index, with the leaves of 3 entries and a root of 12 leaves, replay to
# the same directory and the same chain; so do the splits, merges and rewrites of nested partitions (bounds of one to three key components and an address) on a third index, whose chain
# is two levels deep, and a read of it by a prefix returns the same entries on the standby. Both nodes run with wal_consistency_checking = generic, so that the standby compares every page it replays
# with the image the primary logged.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

ROUNDS=$(knob SIFT_DIRCHAIN_ROUNDS 12 4)
plan $((ROUNDS * 3 + 4))
node_init primary 2 "$WAL_CONF
wal_level = replica
max_wal_senders = 4
wal_consistency_checking = 'generic'
autovacuum = off"
node_start primary || bail "primary start failed"
SHAPE=("SET sift.test_dir_flat_max = 2" "SET sift.test_dir_leaf_max = 3" "SET sift.test_dir_root_max = 12")
sql primary "CREATE EXTENSION pg_sift_test CASCADE" "CREATE TABLE f (v int)" "CREATE INDEX f_idx ON f USING sift_store_test (v)" \
	"CREATE TABLE s (v int)" "CREATE INDEX s_idx ON s USING sift_store_test (v)" "CREATE TABLE n (v int)" "CREATE INDEX n_idx ON n USING sift_store_test (v)" >/dev/null || bail "setup failed"
NEST=("SELECT sift._test_dir_edit('n_idx', 'ins', 0, 3)" "SELECT sift._test_dir_nest('n_idx', 3, 0, '{0,70000,140000,3000000}')" "SELECT sift._test_dir_nest('n_idx', 4, 1, '{70000,70100}')"
	"SELECT sift._test_dir_nest('n_idx', 3, 2, '{0}')" "SELECT sift._test_dir_nest('n_idx', 7, 0, '{5000000}')" "SELECT sift._test_dir_nest('n_idx', 5, 1, '{140000,140001,140002}')"
	"SELECT sift._test_dir_nest('n_idx', 4, 1, '{70100,70101,70102,70103}')" "SELECT sift._test_dir_nest('n_idx', 3, 4, '{0,200}')" "SELECT sift._test_dir_nest('n_idx', 5, 2, '{70103,70104,100000}')"
	"SELECT sift._test_dir_nest('n_idx', 10, 1, '{3000000,3000064}')" "SELECT sift._test_dir_nest('n_idx', 12, 1, '{5000000}')" "SELECT sift._test_dir_edit('n_idx', 'del', 4, 1)" "SELECT sift._test_dir_nest('n_idx', 3, 1, '{0,1,2}')")
NEST_SHAPE=("SET sift.test_dir_flat_max = 2" "SET sift.test_dir_leaf_max = 3" "SET sift.test_dir_root_max = 4")
PER_ROUND=$(( (${#NEST[@]} + ROUNDS - 1) / ROUNDS ))
node_clone primary standby 3 "wal_consistency_checking = 'generic'
hot_standby_feedback = on"
node_start standby || bail "standby start failed"
ok "standby streaming from a base backup"

LIST="SELECT coalesce(string_agg(gen || ':' || ndocs, ',' ORDER BY ord), '') FROM sift._test_dir_list('f_idx')"
SEGS="SELECT coalesce(string_agg(gen || ':' || header || ':' || size, ' ' ORDER BY gen), '') FROM sift._test_segments('s_idx')"
HOLDS="SELECT sift._test_check('s_idx') || '|' || (sift._test_dir_check('f_idx') > 0)"
NESTED="SELECT coalesce(string_agg(ord || ':' || gen, ',' ORDER BY ord), '') || '|' || coalesce((SELECT string_agg(first || ':' || ord, ',' ORDER BY first) FROM sift._test_dir_nested('n_idx')), '') FROM sift._test_dir_list('n_idx')"
PREFIXES="SELECT coalesce(string_agg(p || '=' || (SELECT coalesce(string_agg(gen::text, ',' ORDER BY gen), '') FROM sift._test_dir_prefix('n_idx', d, t)), ' '), '') FROM (VALUES (1, ARRAY[]::int[], -1::int8), (2, ARRAY[17], -1), (3, ARRAY[17, 0], -1), (4, ARRAY[17, 0, 19], 20)) v(p, d, t)"
RANDOM=7
for r in $(seq "$ROUNDS"); do
	for _ in 1 2 3 4; do
		have=$(sql primary "SELECT count(*) FROM sift._test_dir_list('f_idx')")
		stmt="SELECT 1"
		case $((RANDOM % 5)) in
			0 | 1) ((have < 28)) && stmt="SELECT sift._test_dir_edit('f_idx', 'ins', $((RANDOM % (have + 1))), $((1 + RANDOM % 6)))" ;;
			2) ((have > 3)) && stmt="SELECT sift._test_dir_edit('f_idx', 'del', $((RANDOM % (have - 2))), $((1 + RANDOM % 3)))" ;;
			3) ((have > 4)) && stmt="SELECT sift._test_dir_edit('f_idx', 'rev', $((RANDOM % (have - 3))), 4)" ;;
			4) ((have > 0)) && stmt="SELECT sift._test_dir_edit('f_idx', 'set', 0, 1)" ;;
		esac
		sql primary "${SHAPE[@]}" "$stmt" >/dev/null || bail "round $r: $stmt"
	done
	sql primary "${SHAPE[@]}" "SELECT sift._test_seg_write('s_idx', $((200 + r * 13)), $r)" \
		"SELECT sift._test_seg_drop('s_idx', (SELECT gen FROM sift._test_segments('s_idx') ORDER BY gen OFFSET $((r % 3)) LIMIT 1))" \
		"SELECT sift._test_seg_write('s_idx', $((100 + r * 7)), $((r + 100)))" >/dev/null || bail "round $r: segments"
	for ((o = (r - 1) * PER_ROUND; o < r * PER_ROUND && o < ${#NEST[@]}; o++)); do
		sql primary "${NEST_SHAPE[@]}" "${NEST[o]}" >/dev/null || bail "round $r: ${NEST[o]}"
	done
	catchup primary standby || bail "round $r: the standby did not catch up"
	check "round $r: the nested partitions of the third index, their bounds and the reads by a prefix are the primary's on the standby" \
		test "$(sql standby "$NESTED")|$(sql standby "$PREFIXES")" == "$(sql primary "$NESTED")|$(sql primary "$PREFIXES")"
	check "round $r: the standby's directory of $(sql primary "SELECT nsegs FROM sift._test_dir_info('f_idx')") entries equals the primary's" \
		test "$(sql standby "$LIST")" == "$(sql primary "$LIST")"
	out="standby $(sql standby "$SEGS")|$(sql standby "$HOLDS" 2>&1)"$'\n'"primary $(sql primary "$SEGS")|$(sql primary "$HOLDS" 2>&1)"
	check "round $r: the segments of the second index and the chain of the first hold on the standby" \
		test "$(sql standby "$SEGS")|$(sql standby "$HOLDS")" == "$(sql primary "$SEGS")|$(sql primary "$HOLDS")"
done
check "a chain page that is not the one the reader expected is a retry on the standby (40001)" \
	test "$(sql standby "SELECT sift._test_dir_read_stale('f_idx')" 2>&1 | head -1)" == "ERROR:  sift directory recycled during scan, retry"
check "the same page is damage on the primary" \
	grep -q "is corrupted" <<<"$(sql primary "SELECT sift._test_dir_read_stale('f_idx')" 2>&1)"
check "the chain of the first index is chained on both" test "$(sql standby "SELECT chained FROM sift._test_dir_info('f_idx')")|$(sql primary "SELECT chained FROM sift._test_dir_info('f_idx')")" == "t|t"
finish
