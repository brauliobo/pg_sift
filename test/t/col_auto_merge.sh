#!/usr/bin/env bash
# What the churn of a sift_col index leaves to the user (sift.col_auto_merge, on by default): the end of a VACUUM starts a background worker that seals the write
# buffer and promotes it, the incremental work, and never rewrites the index: the units with a dead row stay as they are, sift.col_maintenance_due names the reason, sift.col_maintenance_status
# recommends REINDEX, and the explicit sift.col_maintain call (or REINDEX) rewrites every segment into one. With the setting off the worker does not run either; and with the maintenance lock
# held (a VACUUM stopped at its cleanup gate, a testing build) the call is skipped while queries and inserts go on.
. "$(dirname "$0")/lib.bash"

plan 12
node_init am 20 "autovacuum = off
sift.test_col_net = off
sift.log_min_duration = 0
sift.maintenance_naptime = 200"
node_start am || bail "start failed"
D=$(dir am)
sql am "CREATE EXTENSION pg_sift" "CREATE TABLE facts (id int, key int, kind text, amount numeric(10,2), pad text, h1 bigint DEFAULT (random() * 1e12)::bigint, h2 bigint DEFAULT (random() * 1e12)::bigint, h3 bigint DEFAULT (random() * 1e12)::bigint) WITH (autovacuum_enabled = false)" \
	"INSERT INTO facts SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
	"CREATE INDEX facts_idx ON facts USING sift_col (id, key, kind, amount, h1, h2, h3)" "VACUUM facts" >/dev/null || bail "setup failed"

# churn N: 1% of the rows each updated, deleted and inserted, spread over the table
churn() { sql am "UPDATE facts SET amount = amount + 1 WHERE id % 100 = $1" "DELETE FROM facts WHERE id % 100 = $1 + 50" \
	"INSERT INTO facts SELECT $1 * 100000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 600) g" >/dev/null; }
# state: segments of rows, those with a dead row, rows in the write buffer
state() { sql am "SELECT count(*) FILTER (WHERE kind = 'immutable') || ' segments, ' || count(*) FILTER (WHERE dirty) || ' dirty, ' || coalesce(sum(rows) FILTER (WHERE kind <> 'immutable'), 0) || ' buffered' FROM sift.col_segments('facts_idx')"; }
due() { sql am "SELECT coalesce(sift.col_maintenance_due('facts_idx'), 'not due')"; }
quiet() { [[ $(sql am "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'") == 0 ]]; }
advice() { sql am "SELECT coalesce((SELECT b FROM unnest(blocked_by) b WHERE b LIKE 'REINDEX recommended%'), '') FROM sift.col_maintenance_status('facts_idx')"; }
groups() { sql am "SET sift.enable_agg = $1" "SET max_parallel_workers_per_gather = 0" "SELECT string_agg(x::text, ',' ORDER BY x::text) FROM (SELECT kind, count(*), sum(amount) FROM facts GROUP BY kind) x" | tail -1; }

# 1. on: the VACUUM asks for the worker, which promotes the write buffer and leaves the dirty units for the user
sql am "ALTER DATABASE postgres SET sift.col_auto_merge = on" >/dev/null
BEFORE=$(grep -c 'rewrote (' "$D/postgres.log")
churn 1
sql am "VACUUM facts" >/dev/null
check "the worker promotes the write buffer after the VACUUM" wait_for '[[ $(state) == *" 0 buffered" ]]' 60
check "and ends" wait_for quiet 60
check "no segment was rewritten: units with a dead row are still there" test "$(state | grep -c ' 0 dirty')" == 0
check "no rewrite is in the log" test "$(grep -c 'rewrote (' "$D/postgres.log")" == "$BEFORE"
check "the rewrite is due, and the status recommends REINDEX with the reason" test "$(due)|$(advice)" == "dirty units|REINDEX recommended (dirty units): the server does not rewrite an index by itself, REINDEX INDEX CONCURRENTLY facts_idx does"
check "the answers are the heap's" test "$(groups on)" == "$(groups off)"

# 2. off: the VACUUM leaves the index as its policy does, the call rewrites it
sql am "ALTER DATABASE postgres SET sift.col_auto_merge = off" >/dev/null
churn 2
sql am "VACUUM facts" >/dev/null
check "off: it is due and sift.col_maintain rewrites it" test "$(due) $(sql am "SELECT sift.col_maintain('facts_idx')")" == "dirty units t"
check "off: one clean segment after the call, and nothing is due" test "$(state)|$(due)|$(advice)" == "1 segments, 0 dirty, 0 buffered|not due|"
sql am "ALTER DATABASE postgres SET sift.col_auto_merge = on" >/dev/null

# 3. the maintenance lock held by a VACUUM stopped at its cleanup: the call is skipped, readers and writers go on
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
