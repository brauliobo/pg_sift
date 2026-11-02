#!/usr/bin/env bash
# The aggregate pushdown with a text search as its row selection (SiftAgg, docs/COL-SEARCH.md): the match set of the BM25 index is read
# a chunk of superblocks ahead of the visibility-map bits that vet its rows, so a VACUUM that removes the dead rows of the match set and
# marks their pages all-visible in between must not leave them in the groups. The same for a column source that reads its own liveness
# first (the sift_col index of the table, col/scan/scan.c). The scan waits for an advisory lock before the read of the map
# (sift.test_vis_pause, a testing build), which the test holds until the VACUUM is done: no timing involved.
. "$(dirname "$0")/lib.bash"

plan 9
node_init va 18 "autovacuum = off
sift.async_maintenance = off"
node_start va || bail "start failed"
D=$(dir va)
sql va "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

# race READER N STATEMENT...: the Nth read of the map by READER waits until the statements have run; prints what READER returned
race() {
	local reader=$1 n=$2 hp
	shift 2
	rm -f "$D/locked" "$D/release"
	{ echo "SELECT pg_advisory_lock(20983, 1);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql va >/dev/null &
	hp=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
	sql va "LOAD 'pg_sift'" "SET sift.test_vis_pause = $n" "SET enable_seqscan = off" "SET sift.enable_scan = off" "$reader" >"$D/got" 2>&1 &
	rp=$!
	for _ in $(seq 400); do
		[[ $(sql va "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break
		sleep 0.05
	done
	[[ $(sql va "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] || { touch "$D/release"; bail "the scan did not wait for the map: $(cat "$D/got")"; }
	local s
	for s in "$@"; do sql va "$s" >/dev/null; done
	touch "$D/release"
	wait "$hp" "$rp"
	tail -1 "$D/got"
}

groups() { echo "SELECT string_agg(cat || '=' || c || '/' || s, ',' ORDER BY cat) FROM (SELECT cat, count(*) c, sum(n) s FROM $1 WHERE body @@> 'x7' GROUP BY cat) g"; }
# what the snapshot sees, by the heap and no pushdown
seen() {
	sql va "SET sift.enable_agg = off" "SET sift.enable_scan = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" \
		"SELECT string_agg(cat || '=' || c || '/' || s, ',' ORDER BY cat) FROM (SELECT cat, count(*) c, sum(n) s FROM $1 WHERE body ~ '(^| )x7( |$)' GROUP BY cat) g" | tail -1
}
shape() { sql va "SET enable_seqscan = off" "SET sift.enable_scan = off" "EXPLAIN (COSTS OFF) $(groups "$1")" | grep -c -E "SiftAgg|Search:" ; }
mk() { # TABLE ROWS: the BM25 index after the column index, so that a VACUUM cleans the column index first
	sql va "CREATE TABLE $1 (id int, body text, cat text, n int)" \
		"INSERT INTO $1 SELECT i, 'zzword w' || i || ' x' || (i % 97), 'c' || (i % 7), i % 100 FROM generate_series(1, $2) i" \
		"CREATE INDEX $1_col ON $1 USING sift_col (cat, n)" "CREATE INDEX $1_idx ON $1 USING sift (body) WITH (write_buffer_limit = 64)" \
		"VACUUM ANALYZE $1" >/dev/null || bail "setup $1 failed"
}

mk agg 60000
sql va "DELETE FROM agg WHERE id % 5 = 0" >/dev/null
check "the aggregate runs as SiftAgg with the search" test "$(shape agg)" -ge 2
check "no VACUUM under the scan: the groups are the snapshot's" test "$(race "$(groups agg)" 1 "SELECT 1")" == "$(seen agg)"
check "a VACUUM under the scan: the groups are the snapshot's" test "$(race "$(groups agg)" 1 "VACUUM agg")" == "$(seen agg)"

mk agg2 60000
sql va "DELETE FROM agg2 WHERE id % 7 = 0" "VACUUM agg2" "DELETE FROM agg2 WHERE id % 5 = 0" >/dev/null
check "a dirty segment, a merge and a VACUUM under the scan" test "$(race "$(groups agg2)" 1 "SELECT sift.merge('agg2_idx', true)" "VACUUM agg2")" == "$(seen agg2)"

mk agg3 60000
sql va "DELETE FROM agg3 WHERE id % 5 = 0" >/dev/null
check "a plain count(*) of the matches: a VACUUM under the scan" \
	test "$(race "SELECT count(*) FROM agg3 WHERE body @@> 'x7'" 1 "VACUUM agg3")" == "$(sql va "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*) FROM agg3 WHERE body ~ '(^| )x7( |$)'" | tail -1)"
# a merge that replaces the segment under a scan that began on it (a rollup refresh does the same): the rows are the snapshot's, and the scan, whose segment
# is no longer the directory's (segments_current), takes none of its units as exact: it reads them all instead of answering from their pre-aggregates
mk agg4 60000
sql va "INSERT INTO agg4 SELECT i, 'zzword w' || i, 'c' || (i % 7), i % 100 FROM generate_series(60001, 90000) i" "SELECT sift.col_promote('agg4_col')" "VACUUM agg4" >/dev/null
check "the index has two segments to merge" test "$(sql va "SELECT segments FROM sift.col_index_info('agg4_col')" | tail -1)" -ge 2
q4="SELECT cat, count(*), sum(n) FROM agg4 GROUP BY cat"
x4="EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $q4"
race "$x4" 1 "SELECT 1" >/dev/null
check "no merge under the scan: the units answer from their pre-aggregates" test "$(grep -c -E 'Units: [0-9]+ read.* [1-9][0-9]* from pre-aggregates' "$D/got")" == 1
race "$x4" 1 "SELECT sift.col_merge('agg4_col', true)" >/dev/null
check "a merge under the scan: none of the units answers from its pre-aggregates" test "$(grep -c -E 'Units: [0-9]+ read.* 0 from pre-aggregates' "$D/got")" == 1
check "the indexes verify" test "$(sql va "SELECT sift.verify('agg_idx', true)" "SELECT sift.verify('agg3_idx', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0
finish
