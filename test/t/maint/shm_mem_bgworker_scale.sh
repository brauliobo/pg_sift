#!/usr/bin/env bash
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
# What a server with many sessions and databases keeps in memory and what its maintenance launcher does with nothing to do: the hashes of a unique slab start small and go with DISCARD ALL, the shared
# pools hold all of sift.shared_cache_kb, a server without a launcher gives back the slots of workers that are gone, a launcher with an empty queue wakes for nothing and counts nothing, and a worker
# with nothing to do leaves for a database that has work and no worker.
. "${SIFT_REPO:?}/test/t/lib/common.bash"

plan 5

node_init sm 1 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = on
sift.maintenance_naptime = 100
max_parallel_workers = 2
log_min_messages = debug1"
node_start sm || bail "start failed"
sql sm "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE u (id int, v int)" "CREATE UNIQUE INDEX u_i ON u USING sift_col (id)" >/dev/null || bail "setup failed"
D=$(dir sm)
launcher() { pgrep -P "$(head -1 "$D/data/postmaster.pid")" -f "sift maintenance launcher"; }
switches() { awk '/^voluntary_ctxt_switches/ { print $2 }' "/proc/$(launcher)/status"; }
deferrals() { grep -c "sift maintenance pacer defers" "$D/postgres.log" || true; }
until_us() { local end=$((${EPOCHREALTIME/./} + $1)); wait_for "[[ \${EPOCHREALTIME/./} -ge $end ]]" 10; }

# the launcher of a queue with nothing in it: eight sessions at work, and in the time they take it neither wakes nor asks the pacer
wait_for "[[ \$(sql sm 'SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE '\"'\"'sift maintenance%'\"'\"' AND backend_type <> '\"'\"'sift maintenance launcher'\"'\"'') == 0 ]]" 30
for _ in $(seq 8); do sql sm "SELECT pg_sleep(4)" >/dev/null & done
wait_for "[[ \$(sql sm \"SELECT count(*) FROM pg_stat_activity WHERE query = 'SELECT pg_sleep(4)'\") == 8 ]]" 30
s0=$(switches)
d0=$(deferrals)
until_us 2500000
s1=$(switches)
d1=$(deferrals)
wait
out="launcher wakes: $((s1 - s0)), deferral lines: $((d1 - d0))"
check "an idle launcher under eight busy sessions does not wake or ask the pacer" test "$((s1 - s0))|$((d1 - d0))" == "0|0"

# the hashes of the keys of a unique slab: small for a small slab, in the budget, gone with DISCARD ALL
out=$(sql sm "SET sift.backend_cache_kb = 4096" "INSERT INTO u SELECT g, g FROM generate_series(1, 100) g" "INSERT INTO u VALUES (1000)" "INSERT INTO u VALUES (1001)" \
	"SELECT coalesce(sum(total_bytes), 0) FROM pg_backend_memory_contexts WHERE name = 'sift cache unique slab hashes'" "DISCARD ALL" \
	"SELECT count(*) FROM pg_backend_memory_contexts WHERE name = 'sift cache unique slab hashes'" | paste -sd' ' -)
read -r held left <<<"$out"
check "the hashes of a small slab take little and DISCARD ALL gives them back" test "$((held > 0 && held < 65536))|$left" == "1|0"

# the shared pools hold all of the setting
out=$(sql sm "SET sift.shared_cache_kb = 102400" \
	"SELECT count(*) FILTER (WHERE pool LIKE 'agg%') || '|' || count(*) || '|' || (sum(limit_bytes) = 102400::int8 * 1024)::int FROM sift.shared_memory_pools()")
check "three pools, none dead, that add up to sift.shared_cache_kb" test "$out" == "0|3|1"

# a server without a launcher: the slots of two workers that are gone do not keep the next request from a worker
node_init np 2 "autovacuum = off
sift.async_maintenance = on
sift.max_maint_workers = 2
sift.maintenance_naptime = 500"
node_start np || bail "start failed"
sql np "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int) USING sift_table" "INSERT INTO t SELECT g FROM generate_series(1, 1000) g" >/dev/null || bail "setup failed"
stat() { sql np "SELECT $1 FROM sift._test_maint_stats()"; }
wait_for "[[ \$(stat pending) == 0 ]]" 30
out=$(sql np "SELECT sift._test_maint_workers_hold(2)")
[[ $out == 2 ]] || bail "two worker slots were not held: $out"
sql np "SELECT sift._test_maint_enqueue('t'::regclass::oid, 3)" >/dev/null
check "a request finds the slots of the dead workers free and a worker drains it" wait_for "[[ \$(stat pending) == 0 ]]" 5

# twelve databases with work, two workers: a worker that leaves its queue empty goes to a database without one
node_init md 3 "autovacuum = off
shared_preload_libraries = 'pg_sift'
sift.async_maintenance = on
sift.max_maint_workers = 2
sift.maintenance_naptime = 2000"
node_start md || bail "start failed"
d() { local db=$1; shift; PGOPTIONS="-c client_min_messages=warning" psql -X -At -q -v ON_ERROR_STOP=1 -h "$(dir md)/sock" -p "$(port md)" -U postgres -d "$db" "${@/#/--command=}"; }
sql md "CREATE DATABASE tpl" "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "setup failed"
d tpl "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" "CREATE TABLE t (id int) USING sift_table" >/dev/null || bail "setup failed"
for i in $(seq 12); do sql md "CREATE DATABASE d$i TEMPLATE tpl" >/dev/null || bail "database $i failed"; done
pending() { sql md "SELECT pending FROM sift._test_maint_stats()"; }
for i in $(seq 12); do
	d "d$i" "INSERT INTO t SELECT g FROM generate_series(1, 1000) g" "SELECT sift._test_maint_enqueue('t'::regclass::oid, 3)" >/dev/null || bail "work in d$i failed"
done
wait_for "[[ \$(pending) == 0 ]]" 4
drained=$(pending)
for i in $(seq 12); do sql md "DROP DATABASE d$i" >/dev/null & done
wait
left=$(sql md "SELECT count(*) FROM pg_database WHERE datname ~ '^d[0-9]+$'")
out="pending after the wait: $drained, databases left: $left"
check "the queues of twelve databases drain in 4 s and their databases are dropped" test "$drained|$left" == "0|0"

finish
