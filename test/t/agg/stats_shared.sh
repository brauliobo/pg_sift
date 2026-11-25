#!/usr/bin/env bash
# sift.stats() holds the counters of the whole server, summed over its backends (a table of the shared memory, no shared_preload_libraries), and sift.stats(local => true) those of the session:
# what one session counts the other sees in the totals and not in its own list, sift.stats_reset() leaves the totals alone, and a restart or a crash starts them again (nothing decides on them).
. "$(dirname "$0")/lib.bash"
plan 9

node_init ss 81 "autovacuum = off
sift.async_maintenance = off"
node_start ss || bail "start failed"
sql ss "CREATE EXTENSION pg_sift" "CREATE TABLE sh (id int, grp int, amount int)" \
	"INSERT INTO sh SELECT i, i % 7, i % 100 FROM generate_series(0, 19999) i" \
	"CREATE INDEX sh_col ON sh USING sift_col (id, grp, amount)" "CREATE TABLE plain (g int)" "VACUUM (FREEZE, ANALYZE) sh" >/dev/null || bail "setup failed"

SCAN="SELECT count(*) FROM (SELECT grp, count(*) FROM sh GROUP BY grp) g"
# the scans a list holds: stat LOCAL(true|false)
stat() { echo "SELECT value FROM sift.stats(local => $2) WHERE name = '$1'"; }
# a session that runs n scans and then reads its own list and the totals
session() {
	local n=$1 i q=()
	for ((i = 0; i < n; i++)); do q+=("$SCAN"); done
	sql ss "SET sift.agg_force = on" "SET max_parallel_workers_per_gather = 0" "${q[@]}" "$(stat scans true)" "$(stat scans false)" | tail -n 2 | tr '\n' ' '
}

check "a server where nothing was scanned has no scans in its totals" test "$(sql ss "$(stat scans false)")" == 0
check "a session that ran 3 scans counts 3 in its own list and 3 in the totals" test "$(session 3)" == "3 3 "
check "another session sees them in the totals and has none of its own" test "$(sql ss "$(stat scans true)" "$(stat scans false)" | tr '\n' ' ')" == "0 3 "
check "that session adds its 2 to the 3 of the first: its list 2, the totals 5" test "$(session 2)" == "2 5 "

# the refusals are counted the same way: a query that kept the normal plan in one session is in the totals and in no other session's list
sql ss "LOAD 'pg_sift'" "SET sift.count_refusals = on" "SELECT g, count(*) FROM plain GROUP BY g" >/dev/null
REFUSED="SELECT string_agg(name, ',' ORDER BY name) FROM sift.stats(local => %s) WHERE name LIKE 'refused.%%'"
check "a refusal counted by one session is in the totals" test -n "$(sql ss "$(printf "$REFUSED" false)")"
check "and not in the list of another session" test -z "$(sql ss "$(printf "$REFUSED" true)")"

check "sift.stats_reset() empties the list of the session and leaves the totals as they were" \
	test "$(sql ss "SELECT sift.stats_reset()" "$(stat scans true)" "$(stat scans false)" | tail -n 2 | tr '\n' ' ')" == "0 5 "

node_stop ss; node_start ss || bail "restart failed"
check "a restart starts the totals again" test "$(sql ss "$(stat scans false)")" == 0
sql ss "SET sift.agg_force = on" "$SCAN" >/dev/null
crash ss; node_start ss || bail "restart after the crash failed"
check "a crash starts them again too" test "$(sql ss "$(stat scans false)")" == 0
finish
