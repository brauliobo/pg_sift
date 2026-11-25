#!/usr/bin/env bash
# Format 2 damage under sift.on_corruption = quarantine, with pg_sift preloaded (a) and without shared_preload_libraries (b): one statement meets the damage and fails once; a session that
# connects after it plans on the heap at once and the registry of every backend lists the index (the table is made in the dynamic shared memory registry by the first backend that needs it,
# which is the one that meets the damage when nothing is preloaded); the shared manifest of the index is out of service with the registry, so a reader that never saw the damage cannot be
# served from it; writes and VACUUM skip the index; the flag in the metapage keeps it out after a restart; REINDEX puts it back. The rows are the heap's at every step.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 26
declare -A CONF=([a]="shared_preload_libraries = 'pg_sift'" [b]="shared_preload_libraries = ''") OFFSET=([a]=91 [b]=92)
for m in a b; do
	node_init "q$m" "${OFFSET[$m]}" "${CONF[$m]}
autovacuum = off
sift.shared_cache_kb = 16384
sift.on_corruption = quarantine"
	node_start "q$m" || bail "start of $m failed"
	sql "q$m" "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" \
		"CREATE TABLE h (id int, k int, v int)" "INSERT INTO h SELECT i, i % 7, i FROM generate_series(1, 20000) i" \
		"CREATE INDEX h_idx ON h USING sift_col (id, k, v)" "ANALYZE h" >/dev/null || bail "setup of $m failed"
done

Q='SELECT count(*), sum(v) FROM h WHERE k = 3'
heap() { sql "$1" "SET sift.enable_scan = off" "SET sift.enable_agg = off" "SET enable_indexscan = off" "SET enable_bitmapscan = off" "$Q" | tail -n 1; }
uses_sift() { sql "$1" "EXPLAIN (COSTS OFF) $Q" | grep -qE 'Sift|h_idx'; }
listed() { [[ $(sql "$1" "SELECT count(*) FROM sift.quarantine WHERE index = 'h_idx'::regclass") == 1 ]]; }
writes() { sql "$1" "INSERT INTO h VALUES (20001, 3, 1)" "UPDATE h SET v = v + 1 WHERE id = 20001" "DELETE FROM h WHERE id = 20001" "VACUUM h" >/dev/null 2>&1; }
served() { sql "$1" "SELECT sift._test_shm_manifest_served('h_idx')"; }
persisted() { [[ $(sql "$1" "SELECT persisted FROM sift.quarantine WHERE index = 'h_idx'::regclass") == t ]]; }

for m in a b; do
	n=q$m
	want=$(heap "$n")
	check "$m: the index is planned before the damage" uses_sift "$n"
	check "$m: the index answers as the heap does" test "$(sql "$n" "$Q" | tail -n 1)" == "$want"
	check "$m: a reader is served the manifest of the index from the shared registry" test "$(served "$n")" == t
	sql "$n" "SELECT sift._test_poke('h_idx', 3, 8176, 9, 2)" "CHECKPOINT" >/dev/null || bail "damage of $m failed"
	out=$(sql "$n" "$Q" 2>&1) || true
	check "$m: the statement that meets the damage fails once" grep -q 'XX002\|corrupted' <<<"$out"
	check "$m: a session that connects after it is planned on the heap and gives the rows of the table" test "$(sql "$n" "$Q" 2>&1 | tail -n 1)" == "$want"
	check "$m: the registry of a session that never met the damage lists the index" listed "$n"
	check "$m: writes and VACUUM go on without the index" writes "$n"
	check "$m: the flag in the metapage is written" wait_for "persisted $n" 60
	check "$m: once the index is out of service no reader is served its manifest from the shared registry" test "$(served "$n")" == f
	node_stop "$n"
	node_start "$n" || bail "restart of $m failed"
	check "$m: after a restart the flag keeps the index out of service" persisted "$n"
	check "$m: after a restart the statements are planned on the heap and exact" test "$(sql "$n" "$Q" | tail -n 1)" == "$want" -a "$(uses_sift "$n" && echo sift)" != sift
	sql "$n" "REINDEX INDEX h_idx" "ANALYZE h" >/dev/null
	check "$m: REINDEX takes the index out of the list and puts it back in service" test "$(sql "$n" "SELECT count(*) FROM sift.quarantine WHERE index = 'h_idx'::regclass")" == 0 -a "$(uses_sift "$n" && echo sift)" == sift
	check "$m: the rebuilt index answers as the heap does" test "$(sql "$n" "$Q" | tail -n 1)" == "$want"
	node_stop "$n"
done
finish
