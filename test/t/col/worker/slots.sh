#!/usr/bin/env bash
# The workers of sift are capped by sift.max_maint_workers (4 by default, 2 here, one count for the whole server): with the promotions that three tables asked for held at the
# progress gate of a testing build, two workers run and a parallel query still gets its workers; what was declined says so in one line (for every kind of worker, once an hour,
# with the count in the detail) and is asked for again by the next change; and a worker does not stay connected waiting: DROP DATABASE of a database whose table was just
# promoted by a worker returns at once (it failed after 5 seconds, "being accessed by other users", while the worker napped between its looks).
. "$(dirname "$0")/lib.bash"

plan 8
node_init ws 65 "autovacuum = off
max_worker_processes = 5
max_parallel_workers = 4
max_parallel_workers_per_gather = 2
sift.max_maint_workers = 2
sift.test_col_net = off
sift.maintenance_naptime = 200"
node_start ws || bail "start failed"
D=$(dir ws)
TABLE="(id int, key int, kind text, amount numeric(10,2), pad text, h1 bigint DEFAULT (random() * 1e12)::bigint, h2 bigint DEFAULT (random() * 1e12)::bigint, h3 bigint DEFAULT (random() * 1e12)::bigint)"
sql ws "CREATE EXTENSION pg_sift" "CREATE TABLE big AS SELECT g FROM generate_series(1, 400000) g" "ALTER TABLE big SET (parallel_workers = 2, autovacuum_enabled = off)" "ANALYZE big" >/dev/null || bail "setup failed"
load() { sql $1 "CREATE TABLE $2 $TABLE" "INSERT INTO $2 SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
	"CREATE INDEX $2_idx ON $2 USING sift_col (id, key, kind, amount, h1, h2, h3)" "VACUUM $2" >/dev/null || bail "load of $2 failed"; }
change() { sql ws "UPDATE $1 SET amount = amount + 1 WHERE id % 5 = 0" "DELETE FROM $1 WHERE id % 100 = 50" \
	"INSERT INTO $1 SELECT 100000 + g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 600) g" "VACUUM $1" >/dev/null; }
sifters() { sql ws "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'"; }
launched() { sql ws "SET parallel_setup_cost = 0" "SET parallel_tuple_cost = 0" "SET min_parallel_table_scan_size = 0" \
	"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM big" | sed -n 's/.*Workers Launched: \([0-9]*\).*/\1/p' | head -1; }
buffered() { sql ws "SELECT coalesce(sum(rows), 0) FROM sift.col_segments('$1_idx') WHERE kind <> 'immutable'"; }
promoted() { [[ $(buffered $1) == 0 ]]; }
waiting() { [[ $(sql ws "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 2 ]]; }

# 1. three tables are indexed and changed, which asks for a promotion of each, and each is held at the progress gate: sift takes two slots, the parallel query gets both of its workers
rm -f "$D/locked" "$D/release"
{ echo "SELECT pg_advisory_lock(20983, 12);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql ws >/dev/null &
HOLDER=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
for t in t1 t2 t3; do load ws $t; done
sql ws "ALTER DATABASE postgres SET sift.async_maintenance = on" "ALTER DATABASE postgres SET sift.test_progress_gate = 1" >/dev/null
for t in t1 t2 t3; do change $t; done
check "two workers of sift are held at the gate" wait_for waiting 60
sleep 2
check "sift.max_maint_workers of them are running, not more" test "$(sifters)" == 2
out=$(launched)
check "a parallel query launched both its workers" test "${out:-0}" == 2

# 2. what could not start says so, once, with the count
check "one line says that no worker could be started" test "$(grep -c 'sift: no background worker for promote of index' "$D/postgres.log")" == 1
check "and it names sift.max_maint_workers" bash -c "grep -A1 'sift: no background worker for promote of index' '$D/postgres.log' | grep -q 'sift.max_maint_workers is 2 and 2 are running'"

# 3. the work that was declined is asked for again by the next change, and done once the slots are free
touch "$D/release"
wait "$HOLDER"
sql ws "ALTER DATABASE postgres RESET sift.test_progress_gate" >/dev/null
served() { for t in t1 t2 t3; do change $t; done; promoted t1 && promoted t2 && promoted t3; }
check "the tables that were declined are promoted" wait_for served 120

# 4. a worker that promoted a table does not keep the database connected (the default naptime: it napped 3 times of 2 seconds between its looks)
node_init wd 68 "autovacuum = off
sift.test_col_net = off"
node_start wd || bail "start of wd failed"
sqld() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir wd)/sock" -p "$(port wd)" -U postgres -d "$db" "${@/#/--command=}"; }
sql wd "CREATE DATABASE dd" >/dev/null
sqld dd "CREATE EXTENSION pg_sift" "ALTER DATABASE dd SET sift.async_maintenance = on" >/dev/null || bail "setup of dd failed"
sqld dd "CREATE TABLE t $TABLE" "INSERT INTO t SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
	"CREATE INDEX t_idx ON t USING sift_col (id, key, kind, amount, h1, h2, h3)" "VACUUM t" >/dev/null || bail "load of t failed"
sqld dd "UPDATE t SET amount = amount + 1 WHERE id % 5 = 0" "VACUUM t" >/dev/null
dd_promoted() { [[ $(sqld dd "SELECT coalesce(sum(rows), 0) FROM sift.col_segments('t_idx') WHERE kind <> 'immutable'") == 0 ]]; }
check "the worker promoted the table" wait_for dd_promoted 60
T0=$(date +%s.%N)
out=$(sql wd "DROP DATABASE dd" 2>&1)
T1=$(date +%s.%N)
SECS=$(awk "BEGIN { print $T1 - $T0 }")
diag "DROP DATABASE took $SECS s: ${out:-ok}"
check "DROP DATABASE returns within 5 seconds" bash -c "[[ -z '$out' ]] && awk 'BEGIN { exit !($SECS < 5) }'"
finish
