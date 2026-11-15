#!/usr/bin/env bash
# The slots of max_worker_processes (4 here, one of them taken by the logical replication launcher): the workers of sift never take the last ones, so a parallel query
# still gets its workers while the VACUUMs that three tables asked for are at work (held at the cleanup gate of a testing build); what was declined says so in one line
# (for every kind of worker, once a minute, with the slots in the detail) and is asked for again by the next change; and a worker does not stay connected waiting:
# DROP DATABASE of a database whose table was just vacuumed by a worker returns at once (it failed after 5 seconds, "being accessed by other users", while the
# worker napped between its looks).
. "$(dirname "$0")/lib.bash"

plan 8
node_init ws 65 "autovacuum = off
max_worker_processes = 4
max_parallel_workers = 4
max_parallel_workers_per_gather = 2
sift.test_col_net = off
sift.maintenance_naptime = 200"
node_start ws || bail "start failed"
D=$(dir ws)
TABLE="(id int, key int, kind text, amount numeric(10,2), pad text, h1 bigint DEFAULT (random() * 1e12)::bigint, h2 bigint DEFAULT (random() * 1e12)::bigint, h3 bigint DEFAULT (random() * 1e12)::bigint)"
sql ws "CREATE EXTENSION pg_sift" "CREATE TABLE big AS SELECT g FROM generate_series(1, 400000) g" "ALTER TABLE big SET (parallel_workers = 2, autovacuum_enabled = off)" "ANALYZE big" >/dev/null || bail "setup failed"
load() { sql $1 "CREATE TABLE $2 $TABLE" "INSERT INTO $2 SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
	"CREATE INDEX $2_idx ON $2 USING sift_col (id, key, kind, amount, h1, h2, h3)" >/dev/null || bail "load of $2 failed"; }
change() { sql ws "UPDATE $1 SET amount = amount + 1 WHERE id % 5 = 0" >/dev/null; }
sifters() { sql ws "SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'sift %'"; }
launched() { sql ws "SET parallel_setup_cost = 0" "SET parallel_tuple_cost = 0" "SET min_parallel_table_scan_size = 0" \
	"EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM big" | sed -n 's/.*Workers Launched: \([0-9]*\).*/\1/p' | head -1; }
vacuumed() { [[ $(sql ws "SELECT vacuum_count FROM pg_stat_user_tables WHERE relname = '$1'") -gt 0 ]]; }
waiting() { [[ $(sql ws "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") -ge 1 ]]; }

# 1. three tables are loaded and indexed, which asks for a VACUUM of each at the commit, and each VACUUM is held at the cleanup gate: sift takes one slot, the parallel query gets both of its workers
rm -f "$D/locked" "$D/release"
{ echo "SELECT pg_advisory_lock(20983, 2);"; echo "\\! touch '$D/locked'"; echo "\\! while [ ! -e '$D/release' ]; do sleep 0.05; done"; } | sql ws >/dev/null &
HOLDER=$!
until [[ -e $D/locked ]]; do sleep 0.05; done
sql ws "ALTER DATABASE postgres SET sift.col_auto_merge = on" "ALTER DATABASE postgres SET sift.test_cleanup_gate = on" >/dev/null
for t in t1 t2 t3; do load ws $t; done
check "a worker of sift is held at the gate" wait_for waiting 60
sleep 2
check "sift took one of the slots, not all of them" test "$(sifters)" == 1
out=$(launched)
check "a parallel query launched both its workers" test "${out:-0}" == 2

# 2. what could not start says so, once, with the slots
check "one line says that no worker could be started" test "$(grep -c 'sift: no background worker for vacuum of index' "$D/postgres.log")" == 1
check "and it names max_worker_processes" bash -c "grep -A1 'sift: no background worker for vacuum of index' '$D/postgres.log' | grep -q 'max_worker_processes is 4'"

# 3. the work that was declined is asked for again by the next change, and done once the slot is free
touch "$D/release"
wait "$HOLDER"
sql ws "ALTER DATABASE postgres RESET sift.test_cleanup_gate" >/dev/null
served() { for t in t1 t2 t3; do change $t; done; vacuumed t1 && vacuumed t2 && vacuumed t3; }
check "the tables that were declined are vacuumed" wait_for served 120

# 4. a worker that vacuumed a table does not keep the database connected (the default naptime: it napped 3 times of 2 seconds between its looks)
node_init wd 68 "autovacuum = off
sift.test_col_net = off"
node_start wd || bail "start of wd failed"
sqld() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir wd)/sock" -p "$(port wd)" -U postgres -d "$db" "${@/#/--command=}"; }
sql wd "CREATE DATABASE dd" >/dev/null
sqld dd "CREATE EXTENSION pg_sift" "ALTER DATABASE dd SET sift.col_auto_merge = on" >/dev/null || bail "setup of dd failed"
sqld dd "CREATE TABLE t $TABLE" "INSERT INTO t SELECT g, g % 1000, 'cat_' || (g % 8), g % 997, repeat('x', 150) FROM generate_series(1, 20000) g" \
	"CREATE INDEX t_idx ON t USING sift_col (id, key, kind, amount, h1, h2, h3)" >/dev/null || bail "load of t failed"
dd_vacuumed() { [[ $(sqld dd "SELECT vacuum_count FROM pg_stat_user_tables WHERE relname = 't'") -ge 1 ]]; }
check "the worker vacuumed the table" wait_for dd_vacuumed 60
T0=$(date +%s.%N)
out=$(sql wd "DROP DATABASE dd" 2>&1)
T1=$(date +%s.%N)
SECS=$(awk "BEGIN { print $T1 - $T0 }")
diag "DROP DATABASE took $SECS s: ${out:-ok}"
check "DROP DATABASE returns within 5 seconds" bash -c "[[ -z '$out' ]] && awk 'BEGIN { exit !($SECS < 5) }'"
finish
