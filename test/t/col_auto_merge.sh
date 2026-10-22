#!/usr/bin/env bash
# The automatic rewrite of a sift_col index the churn left scattered (sift.col_auto_merge, off by default): the end of a VACUUM that finds the rule satisfied
# (units with a dead row, units listed per superblock, rows in the write buffer) starts a background worker that seals the write buffer and merges
# every segment into one, the way sift.col_maintain does in a session. Here: the worker runs after the VACUUM; with it off it does
# not, and the explicit call still does; it sleeps while it writes as sift.maintenance_cost_delay says (the time of its rewrite, in the log, has a
# lower bound set by the pages it writes); and with the maintenance lock held (a VACUUM stopped at its cleanup gate, a testing build) it is skipped while queries and inserts go on.
. "$(dirname "$0")/lib.bash"

plan 12
node_init am 20 "autovacuum = off
sift.log_min_duration = 0"
node_start am || bail "start failed"
D=$(dir am)
sql am "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 150000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount)" "VACUUM facts" >/dev/null || bail "setup failed"

# churn N: 1% of the rows each updated, deleted and inserted, spread over the table
churn() { sql am "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $1" "DELETE FROM facts WHERE id % 100 = $1 + 50" \
	"INSERT INTO facts SELECT $1 * 100000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 600) g" >/dev/null; }
# state: segments of rows, those with a dead row, rows in the write buffer
state() { sql am "SELECT count(*) FILTER (WHERE kind = 'immutable') || ' segments, ' || count(*) FILTER (WHERE dirty) || ' dirty, ' || coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0) || ' buffered' FROM sift.col_segments('facts_idx')"; }
due() { sql am "SELECT coalesce(sift.col_maintenance_due('facts_idx'), 'not due')"; }
scattered() { [[ $(state) != "1 segments, 0 dirty, 0 buffered" ]]; }
await_rewrite() { # until the index is one clean segment
	for _ in $(seq 400); do scattered || return 0; sleep 0.05; done
	return 1
}
groups() { sql am "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1; }

# 1. on (it is off by default): the VACUUM asks for it, the worker does it
sql am "ALTER DATABASE postgres SET sift.col_auto_merge = on" >/dev/null
churn 1
sql am "VACUUM facts" >/dev/null
check "the worker rewrites it after the VACUUM: one segment, no dead row, nothing in the buffer" await_rewrite
check "the index verifies and the answers are the table's" test "$(sql am "SELECT sift.col_verify('facts_idx', true)" 2>&1 | grep -c ERROR)" == 0
check "the answers are the heap's" test "$(groups on)" == "$(groups off)"
check "nothing is due after it" test "$(due)" == "not due"

# 2. off (the default): the VACUUM leaves the index as its policy does, the call rewrites it
sql am "ALTER DATABASE postgres SET sift.col_auto_merge = off" >/dev/null
churn 2
sql am "VACUUM facts" >/dev/null
sleep 2
check "off: no rewrite after the VACUUM" scattered
check "off: it is due and sift.col_maintain rewrites it" test "$(due) $(sql am "SELECT sift.col_maintain('facts_idx')")" == "dirty units t"
check "off: one clean segment after the call" test "$(state)" == "1 segments, 0 dirty, 0 buffered"
sql am "ALTER DATABASE postgres SET sift.col_auto_merge = on" >/dev/null

# 3. the worker sleeps as sift.maintenance_cost_delay says while it writes: a rewrite of the pages it writes cannot take less than their delays
sql am "ALTER DATABASE postgres SET sift.maintenance_cost_delay = 100" >/dev/null
REWROTE='rewrote (.*) the segments of index "facts_idx" in [0-9]* ms'
BEFORE=$(grep -c "$REWROTE" "$D/postgres.log")
churn 3
sql am "VACUUM facts" >/dev/null
await_rewrite
for _ in $(seq 400); do [[ $(grep -c "$REWROTE" "$D/postgres.log") -gt $BEFORE ]] && break; sleep 0.05; done
MS=$(grep "$REWROTE" "$D/postgres.log" | tail -1 | grep -o '[0-9]* ms' | grep -o '[0-9]*')
PAGES=$(sql am "SELECT (sum(bytes) / 8192)::int FROM sift.col_segments('facts_idx')")
BOUND=$(( (PAGES / 16 - 2) * 100 ))
diag "the last rewrite took ${MS:-?} ms for $PAGES pages at 100 ms for every 16 pages (at least $BOUND ms)"
sql am "ALTER DATABASE postgres RESET sift.maintenance_cost_delay" >/dev/null
check "the worker slept while it wrote" test "${MS:-0}" -ge "$BOUND" -a "$BOUND" -ge 200

# 4. the maintenance lock held by a VACUUM stopped at its cleanup: the call is skipped, readers and writers go on
churn 4
rm -f "$D/locked" "$D/release"
{
	echo "SELECT pg_advisory_lock(20983, 2);"
	echo "\\! touch '$D/locked'"
	echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"
} | sql am >/dev/null &
hp=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sql am "LOAD 'pg_sift'" "SET sift.test_cleanup_gate = on" "SET sift.col_auto_merge = off" "VACUUM facts" >"$D/vacuum" 2>&1 &
vp=$!
for _ in $(seq 400); do [[ $(sql am "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]] && break; sleep 0.05; done
skipped=$(timeout 20 psql -X -At -q -h "$D/sock" -p "$(port am)" -U postgres -d postgres -c "SELECT sift.col_maintain('facts_idx')" | tail -1)
served=$(timeout 20 psql -X -At -q -h "$D/sock" -p "$(port am)" -U postgres -d postgres -c "SELECT count(*) > 0 FROM facts WHERE id < 100" \
	-c "INSERT INTO facts SELECT 900000 + g, g, 'cat_0', 1, 'p' FROM generate_series(1, 50) g" -c "UPDATE facts SET amount = 3 WHERE id = 5" | head -1)
touch "$D/release"
wait "$vp" "$hp"
check "maintenance lock held: sift.col_maintain is skipped" test "$skipped" == f
check "maintenance lock held: a query, an insert and an update are served" test "$served" == t
check "the VACUUM ended well and the rewrite is still due" test "$(grep -c ERROR "$D/vacuum")|$(due)" == "0|dirty units"
sql am "SELECT sift.col_maintain('facts_idx')" >/dev/null
check "afterwards the call rewrites it and the answers are the heap's" test "$(state)|$(groups on)" == "1 segments, 0 dirty, 0 buffered|$(groups off)"
finish
