#!/usr/bin/env bash
# A VACUUM that runs while a count(*) of the index is between the liveness it has read and the visibility-map bits it is about to read.
# The count must be what the snapshot of the query sees: a tuple deleted before the snapshot and removed by that VACUUM is not one of the
# rows, however the VACUUM leaves the heap pages (all-visible) and the liveness of the segment. A VACUUM clears the index entries of the
# dead tuples first and marks the pages all-visible after; a count that read the liveness (or the directory's dirty flag of the segment,
# or the segments themselves) before the bit has to give the pages to the heap. The scan waits for an advisory lock before the read of the
# map (sift.test_vis_pause, a testing build), which the test holds until the VACUUM is done: no timing involved.
. "$(dirname "$0")/lib.bash"

plan 10
node_init vc 16 "autovacuum = off
sift.async_maintenance = off"
node_start vc || bail "start failed"
D=$(dir vc)
sql vc "CREATE EXTENSION pg_sift" >/dev/null || bail "extension failed"

# race READER N STATEMENT...: the Nth read of the map by READER waits until the statements have run; prints what READER returned
race() {
	local reader=$1 n=$2 hp
	shift 2
	rm -f "$D/locked" "$D/release"
	{ echo "SELECT pg_advisory_lock(20983, 1);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql vc >/dev/null &
	hp=$!
	until [[ -e $D/locked ]]; do sleep 0.05; done
	sql vc "LOAD 'pg_sift'" "SET sift.test_vis_pause = $n" "SET enable_seqscan = off" "$reader" >"$D/got" 2>&1 &
	rp=$!
	until [[ $(sql vc "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]]; do sleep 0.05; done
	local s
	for s in "$@"; do sql vc "$s" >/dev/null; done
	touch "$D/release"
	wait "$hp" "$rp"
	tail -1 "$D/got"
}

# seen T: what the snapshot sees, by a sequential scan
seen() { sql vc "SET enable_indexscan = off" "SET enable_bitmapscan = off" "SELECT count(*) FROM $1 WHERE body LIKE '%zzword%'" | tail -1; }
counter() { echo "SELECT count(*) FROM $1 WHERE body @@> 'zzword'"; }
# the plan with no column of the table: an unordered scan of the matches whose visibility the map decides
pages() { echo "SELECT count(*) FROM (SELECT 1 FROM $1 WHERE body @@> 'zzword' OFFSET 0) s"; }
mk() { # TABLE ROWS SEGMENTS
	sql vc "CREATE TABLE $1 (id int, body text)" "INSERT INTO $1 SELECT i, 'zzword w' || i FROM generate_series(1, $2) i" \
		"CREATE INDEX $1_idx ON $1 USING sift (body) WITH (mutable_limit = 64, build_segments = $3)" "VACUUM $1" >/dev/null || bail "setup $1 failed"
}
verifies() { test "$(sql vc "SELECT sift.verify('$1_idx', true)" 2>&1 | grep -c -E "ERROR|corrupt")" == 0; }

# a segment that was clean when the scan began: the directory it read does not flag it dirty
mk clean 20000 1
sql vc "DELETE FROM clean WHERE id % 5 = 0" >/dev/null
check "no VACUUM under the scan: the count is the snapshot's" test "$(race "$(counter clean)" 1 "SELECT 1")" == "$(seen clean)"
check "clean segment: a VACUUM under the scan" test "$(race "$(counter clean)" 1 "VACUUM clean")" == "$(seen clean)"
mk pages 20000 1
sql vc "DELETE FROM pages WHERE id % 5 = 0" >/dev/null
check "clean segment, matches read page by page: a VACUUM under the scan" test "$(race "$(pages pages)" 1 "VACUUM pages")" == "$(seen pages)"
check "the index verifies" verifies clean

# a segment already dirty, the dirty bit of the second superblock clean when the scan loaded the bitmap at the first
mk dirty 100000 1
sql vc "DELETE FROM dirty WHERE id % 7 = 0 AND id < 10000" "VACUUM dirty" "DELETE FROM dirty WHERE id % 5 = 0 AND id > 40000" >/dev/null
check "dirty segment, a superblock dirtied under the scan" test "$(race "$(counter dirty)" 2 "VACUUM dirty")" == "$(seen dirty)"
check "the index verifies" verifies dirty

# a merge replaces the segments under the scan, and the VACUUM after it clears the bits of the new ones only
mk merged 60000 4
sql vc "DELETE FROM merged WHERE id % 7 = 0" "VACUUM merged" "DELETE FROM merged WHERE id % 5 = 0" >/dev/null
check "a merge and a VACUUM under the scan" test "$(race "$(counter merged)" 1 "SELECT sift.merge('merged_idx', true)" "VACUUM merged")" == "$(seen merged)"
check "the index verifies" verifies merged

# a DELETE, a VACUUM and a promotion of the mutable segment under the scan
sql vc "CREATE TABLE lane (id int, body text)" "CREATE INDEX lane_idx ON lane USING sift (body) WITH (mutable_limit = 65536)" \
	"INSERT INTO lane SELECT i, 'zzword w' || i FROM generate_series(1, 20000) i" "VACUUM lane" "DELETE FROM lane WHERE id % 5 = 0" >/dev/null || bail "setup lane failed"
check "mutable segment: a VACUUM and a promotion under the scan" test "$(race "$(counter lane)" 1 "VACUUM lane" "SELECT sift.promote('lane_idx')")" == "$(seen lane)"

# an undisturbed count keeps trusting the map: the page checks only follow a directory that moved
heap_pages() { sql vc "SET enable_seqscan = off" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $1" | sed -n 's/^ *Heap Pages: //p'; }
mk calm 20000 1
sql vc "VACUUM calm" >/dev/null
check "no directory change under the count: the map decides every page" test "$(heap_pages "$(counter calm)")" == 0
finish
