#!/usr/bin/env bash
# The table of the leaf statistics that backends share (col/leafshm.c, sift.col_leaf_cache_kb): the first backend that plans over a sift_col index folds the zones of
# its units into the statistics of the planner, the next backends take them from it and plan the same statement to the same plan; a rewrite of the index (a new
# stamp) makes the next backend fold them again; the setting at 0 is the off switch; the table does not outlive a crash restart.
((BASH_VERSINFO[0] >= 4)) || { echo 'needs bash 4+ (brew install bash)' >&2; exit 1; }
. "$(dirname "$0")/lib.bash"

plan 8
node_init primary 2
node_start primary || bail "primary start failed"
sql primary <<'SQL' >/dev/null || bail "setup failed"
CREATE EXTENSION pg_sift;
CREATE TABLE events (id int PRIMARY KEY, set_id int NOT NULL, kind_id int NOT NULL, ts date NOT NULL, amount float8);
INSERT INTO events SELECT g, 1 + g % 30, 1 + g % 20, date '2025-01-01' + (g * 7919 % 360), g % 100 FROM generate_series(1, 30000) g;
CREATE INDEX events_col ON events USING sift_col (id, set_id, kind_id, ts, amount);
VACUUM (FREEZE, ANALYZE) events;
CREATE FUNCTION plan_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; t text := '';
BEGIN FOR l IN EXECUTE 'EXPLAIN ' || q LOOP t := t || l || E'\n'; END LOOP; RETURN md5(t); END $$;
SQL

# a new backend: the plan of a statement as an md5, then the leaf statistics it built and took
backend() {
	local node=$1 pre=${2:-SET sift.enable_agg = on}
	sql "$node" "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "$pre" \
		"SELECT plan_md5('SELECT kind_id, sum(amount), count(*) FROM events WHERE set_id IN (3, 4) AND ts >= date ''2025-06-01'' GROUP BY kind_id')" \
		"SELECT string_agg(value::text, ',' ORDER BY name) FROM sift.stats() WHERE name IN ('leaf_stats_built', 'leaf_stats_shared')" | tr '\n' ' ' | sed 's/ $//'
}
BUILT='1,0'
SHARED='0,1'
stat() { echo "${1#* }"; }
plan_of() { echo "${1%% *}"; }

# the build and the VACUUM of the setup folded them in their backends; a restart empties the table
node_stop primary
node_start primary || bail "restart failed"
A=$(backend primary)
check "the first backend folds the statistics" test "$(stat "$A")" == "$BUILT"
B=$(backend primary)
check "a backend that connects later takes them" test "$(stat "$B")" == "$SHARED"
check "and plans to the plan of the first" test "$(plan_of "$B")" == "$(plan_of "$A")"
C=$(backend primary "SET sift.col_leaf_cache_kb = 0")
check "the setting at 0 folds them, to the same plan" test "$(stat "$C")|$(plan_of "$C")" == "$BUILT|$(plan_of "$A")"

# a rewrite of the index moves the stamp of its segments: the statistics of the old ones are not read
sql primary "INSERT INTO events SELECT g, 1 + g % 30, 1 + g % 20, date '2025-01-01' + (g * 7919 % 360), g % 100 FROM generate_series(30001, 60000) g" "SELECT sift.col_merge('events_col', true)" "ANALYZE events" >/dev/null || bail "merge failed"
D=$(backend primary)
E=$(backend primary)
check "after the index changed the first backend folds them again" test "$(stat "$D")" == "$BUILT"
check "and the next takes those, to the same plan" test "$(stat "$E")|$(plan_of "$E")" == "$SHARED|$(plan_of "$D")"

# the table is in the registry of the server: the crash restart of a backend that is killed (every backend goes down, the shared memory is made again) discards it
node_conf primary "restart_after_crash = on"
sql primary "SELECT pg_reload_conf()" >/dev/null
sql primary "SELECT pg_sleep(600)" >/dev/null 2>&1 &
victim() { sql primary "SELECT pid FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()"; }
for _ in $(seq 300); do [[ -n $(victim) ]] && break; sleep 0.1; done
SIFT_TMP=$(dir primary) leak_expect 1 'terminated by signal 9'
kill -9 "$(victim)"
wait
for _ in $(seq 300); do sql primary "SELECT 1" >/dev/null 2>&1 && break; sleep 0.1; done
F=$(backend primary)
G=$(backend primary)
check "after a crash restart the first backend folds them" test "$(stat "$F")" == "$BUILT"
check "and the next takes them, to the plan from before" test "$(stat "$G")|$(plan_of "$G")" == "$SHARED|$(plan_of "$D")"
finish
