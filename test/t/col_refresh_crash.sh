#!/usr/bin/env bash
# sift.col_refresh (docs/COL-PREAGG.md section 7) and a crash: the rewrite of a segment is one atomic replacement of the directory, so a PANIC after the
# new segment is written and before it replaces the old one, a cancel between two segments, and a kill -9 of the whole cluster at random points of a refresh (three times, the options
# switched each time), leave the index as it was or with the segments done: it verifies, its rows and answers are the heap's, and the refresh made
# again completes and has nothing left to do. The options are switched back and forth so that each refresh has every segment to rewrite.
. "$(dirname "$0")/lib.bash"

plan 25
node_init primary 2 "autovacuum = off
sift.async_maintenance = off"
node_start primary || bail "primary start failed"
ROWS=$(knob SIFT_ROWS 60000 30000)
sql primary "CREATE EXTENSION pg_sift_test CASCADE" \
	"CREATE TABLE items (id int, kind int, grp text, val bigint, amt numeric(10,2), pad text)" \
	"INSERT INTO items SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), CASE WHEN g % 7 = 0 THEN NULL ELSE (g * 977) % 100000 - 50000 END,
		((g * 91) % 20000) / 100.0 - 50, repeat('p', 100) FROM generate_series(1, $ROWS / 2) g" \
	"CREATE INDEX items_idx ON items USING sift_col (id, kind, grp, val, amt) WITH (write_buffer_limit = 4096, cube_minmax = false, superunit = 0)" >/dev/null || bail "setup failed"
for i in 1 2 3 4; do
	sql primary "INSERT INTO items SELECT g, g % 6, 'g' || lpad((g % 30)::text, 2, '0'), (g * 977) % 100000 - 50000, ((g * 91) % 20000) / 100.0 - 50, repeat('p', 100)
		FROM generate_series($ROWS / 2 + ($i - 1) * $ROWS / 8 + 1, $ROWS / 2 + $i * $ROWS / 8) g" "SELECT sift.col_promote('items_idx')" >/dev/null || bail "promote $i failed"
done
sql primary "VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) items" >/dev/null

Q="SELECT kind, min(val), max(val), min(amt), max(amt), count(*) FROM items GROUP BY kind"
SETTINGS=("LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "SET seq_page_cost = 10000")
answer() { sql primary "${SETTINGS[@]}" "SET sift.enable_agg = $1" "SELECT md5(string_agg(x::text, ',' ORDER BY x::text)) FROM ($Q) x" | tail -1; }
# the units the groups answered, of all the units
plan_of() {
	local out
	out=$(sql primary "${SETTINGS[@]}" "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $Q")
	for f in 'Units' 'Units From Pre-aggregates'; do
		grep -E "^\s*$f: [0-9]+" <<<"$out" | grep -oE '[0-9]+$' || echo 0
	done | paste -sd' '
}
SEGS="SELECT string_agg(gen || ':' || coalesce(rows, 0), ' ' ORDER BY gen) FROM sift.col_segments('items_idx') WHERE kind = 'immutable'"
ROWSUM="SELECT sum(rows) FROM sift.col_segments('items_idx') WHERE kind = 'immutable'"
verify() { test -z "$(sql primary "SELECT sift.col_verify('items_idx', true)" 2>&1)"; }
refresh() { sql primary "SELECT refreshed || ' ' || remaining FROM sift.col_refresh('items_idx')"; }
stale() { sql primary "SELECT remaining FROM sift.col_refresh('items_idx', 0)"; }

WANT=$(answer off)
BASE_ROWS=$(sql primary "$ROWSUM")
check "the heap's answer and the scanned one agree before the option is set" test "$(answer on)" == "$WANT"
check "nothing is stale: the options and the segments agree" test "$(stale)" == 0

# the PANIC: the new segment is written, the old one not yet replaced
sql primary "ALTER INDEX items_idx SET (cube_minmax = true)" >/dev/null
N=$(stale)
check "the option leaves every segment stale" test "$N" -ge 4
BEFORE=$(sql primary "$SEGS")
SIFT_TMP=$(dir primary) leak_expect 1 'PANIC:'
SIFT_TMP=$(dir primary) leak_expect 1 'terminated by signal 6'
node_conf primary "sift.test_col_rollup_panic = 1"
pg_ctl -D "$(dir primary)/data" reload >/dev/null
sleep 1
refresh_panics() { ! sql primary "SELECT sift.col_refresh('items_idx')" >/dev/null 2>&1; }
check "the refresh PANICs with the new segment written" refresh_panics
for _ in $(seq 300); do [[ -f $(dir primary)/data/postmaster.pid ]] || break; sleep 0.1; done
sed -i '/^sift.test_col_rollup_panic/d' "$(dir primary)/data/postgresql.conf"
check "the primary restarts through crash recovery" node_start primary
check "the index verifies after the crash" verify
check "its segments are those it had" test "$(sql primary "$SEGS")" == "$BEFORE"
check "the answer is the heap's, still scanned" test "$(answer on)|$(plan_of | cut -d' ' -f2)" == "$WANT|0"
check "the refresh made again rewrites every segment and has none left" test "$(refresh)" == "$N 0"
check "the groups answer, with the same answer" test "$(answer on)" == "$WANT"
check "it verifies and the rows are the same" test "$(verify && echo ok)|$(sql primary "$ROWSUM")" == "ok|$BASE_ROWS"
check "a second call has nothing to do" test "$(refresh)" == "0 0"
SIZE=$(sql primary "SELECT pg_relation_size('items_idx')")

# a cancel between two segments: the ones done stay, the call made again goes on
sql primary "ALTER INDEX items_idx SET (cube_minmax = false)" >/dev/null
sql primary "SET sift.test_col_refresh_sleep_ms = 300" "SELECT * FROM sift.col_refresh('items_idx')" >/dev/null 2>&1 &
BG=$!
sleep 0.9
sql primary "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE query LIKE '%col_refresh(%' AND pid <> pg_backend_pid()" >/dev/null
wait "$BG" 2>/dev/null || true
left=$(stale)
check "a cancel leaves some segments done and some to do ($left of $N left)" test "$left" -ge 1 -a "$left" -lt "$N"
check "after the cancel the index verifies, its rows and answers are the heap's" test "$(verify && echo ok)|$(sql primary "$ROWSUM")|$(answer on)" == "ok|$BASE_ROWS|$WANT"
check "the call made again does the rest" test "$(refresh | cut -d' ' -f1)|$(stale)" == "$left|0"

# kill -9 at random points of three refreshes
for i in 1 2 3; do
	if ((i % 2)); then OPT=true; else OPT=false; fi
	sql primary "ALTER INDEX items_idx SET (cube_minmax = $OPT)" >/dev/null
	# each segment waits 250 ms between its write and its replacement: the kill falls inside the rewrite of one of them
	sql primary "SET sift.test_col_refresh_sleep_ms = 250" "SELECT * FROM sift.col_refresh('items_idx')" >/dev/null 2>&1 &
	BG=$!
	sleep "0.$((RANDOM % 9 + 1))$((RANDOM % 10))"
	crash primary
	wait "$BG" 2>/dev/null || true
	node_start primary || bail "cycle $i: restart failed"
	left=$(stale)
	check "cycle $i: the kill fell inside the refresh ($left of $N segments were left to do)" test "$left" -ge 1
	check "cycle $i: after kill -9 the index verifies, its rows and answers are the heap's" \
		test "$(verify && echo ok)|$(sql primary "$ROWSUM")|$(answer on)" == "ok|$BASE_ROWS|$WANT"
	check "cycle $i: the refresh made again completes, and verifies" test "$(refresh | cut -d' ' -f2)|$(stale)|$(verify && echo ok)" == "0|0|ok"
done
sql primary "VACUUM items" >/dev/null
AFTER=$(sql primary "SELECT pg_relation_size('items_idx')")
diag "index file: $SIZE bytes after the first refresh, $AFTER after three killed ones and VACUUM"
check "the killed refreshes left no pages behind that VACUUM does not reuse (the file is under twice the size)" test "$AFTER" -lt $((SIZE * 2))
finish
