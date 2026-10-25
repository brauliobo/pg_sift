#!/usr/bin/env bash
# The table of the planner's verdicts that backends share (agg/verdict.c, sift.verdict_cache_kb): a new backend plans a statement with a shared CTE
# from the verdict another backend made; two backends that plan the same new statement at once (one held at the gate between its lookup and its plan
# in full) both succeed and leave one consistent entry, with the same plan; the table does not outlive a crash restart; a hot standby shares it among its
# backends; the setting at 0 is the off switch. (A table that cannot be had is test/sql/agg_cte_verdict_cache.sql.)
. "$(dirname "$0")/lib.bash"

plan 12
node_init primary 2 "wal_level = replica
max_wal_senders = 4"
node_start primary || bail "primary start failed"
sql primary <<'SQL' >/dev/null || bail "setup failed"
CREATE EXTENSION pg_sift;
CREATE TABLE kinds (id int PRIMARY KEY, enabled bool NOT NULL);
INSERT INTO kinds SELECT g, g <= 20 FROM generate_series(1, 3000) g;
CREATE TABLE events (id int PRIMARY KEY, set_id int NOT NULL, kind_id int NOT NULL, ts date NOT NULL, doc jsonb);
INSERT INTO events SELECT g, 1 + g % 30, 1 + g % 20, date '2025-01-01' + (g * 7919 % 360),
	jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'c' || (g + j) % 4, 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 3) j))
	FROM generate_series(1, 1500) g;
CREATE INDEX events_col ON events USING sift_col (id, set_id, kind_id, ts, doc) WITH (paths = 'doc.parts[]{cat astext, score float8}');
VACUUM (FREEZE, ANALYZE) events;
ANALYZE kinds;
CREATE FUNCTION series_query(key_sets int) RETURNS text LANGUAGE sql AS $f$
	SELECT format($q$WITH r AS (SELECT e.* FROM events e INNER JOIN kinds k ON k.id = e.kind_id AND k.enabled IS TRUE WHERE e.set_id <= %s),
	a AS (SELECT r.ts, o.cat, avg(o.score) AS s, count(DISTINCT r.id) AS n FROM r, jsonb_to_recordset(r.doc['parts']) AS o(cat text, score float) GROUP BY r.ts, o.cat),
	b AS (SELECT r.ts, count(DISTINCT r.id) AS n, avg(o.score) AS s FROM r, jsonb_to_recordset(r.doc['parts']) AS o(cat text, score float) GROUP BY r.ts),
	c AS (SELECT r.ts, r.set_id, count(DISTINCT r.id) AS n FROM r GROUP BY r.ts, r.set_id)
SELECT 'a' AS src, ts, cat, s, n FROM a UNION ALL SELECT 'b', ts, NULL, s, n FROM b UNION ALL SELECT 'c', ts, set_id::text, NULL, n FROM c$q$, key_sets) $f$;
CREATE FUNCTION plan_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; t text := '';
BEGIN FOR l IN EXECUTE 'EXPLAIN ' || q LOOP t := t || l || E'\n'; END LOOP; RETURN md5(t); END $$;
SQL

# a new backend: the plan of the statement as an md5, then hits,misses,shared hits of the session
backend() {
	local node=$1 pre=${2:-SET sift.enable_agg = on}
	sql "$node" "LOAD 'pg_sift'" "SET max_parallel_workers_per_gather = 0" "$pre" "SELECT plan_md5(series_query(10))" \
		"SELECT string_agg(value::text, ',' ORDER BY name) FROM sift.stats() WHERE name IN ('verdict_cache_hits', 'verdict_cache_misses', 'verdict_cache_shared_hits')" | tr '\n' ' ' | sed 's/ $//'
}
FULL='0,1,0'
SHARED='1,0,1'
stat() { echo "${1#* }"; }
plan_of() { echo "${1%% *}"; }

A=$(backend primary)
check "the first backend plans in full" test "$(stat "$A")" == "$FULL"
B=$(backend primary)
check "a backend that connects later plans from its verdict" test "$(stat "$B")" == "$SHARED"
check "and gets the plan of the first" test "$(plan_of "$B")" == "$(plan_of "$A")"
C=$(backend primary "SET sift.verdict_cache_kb = 0")
check "the setting at 0 plans in full, and the plan is the same" test "$(stat "$C")|$(plan_of "$C")" == "$FULL|$(plan_of "$A")"

# two backends meet the same new statement (a REINDEX moves the key); the first waits at the gate after its lookup, the second plans and keeps the verdict
sql primary "REINDEX INDEX events_col" >/dev/null
coproc HOLD { sql primary; }
echo "SELECT pg_advisory_lock(20984, 1);" >&"${HOLD[1]}"
until [[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND granted") == 1 ]]; do sleep 0.1; done
backend primary "SET sift.test_verdict_gate = on" >"$(dir primary)/gated.out" 2>&1 &
GATED=$!
until [[ $(sql primary "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND NOT granted") == 1 ]]; do sleep 0.1; done
D=$(backend primary)
check "the statement is planned in full by the backend that was not held" test "$(stat "$D")" == "$FULL"
echo "SELECT pg_advisory_unlock(20984, 1);" >&"${HOLD[1]}"
check "the held backend, which also planned in full, finishes without an error" wait "$GATED"
E1=$(cat "$(dir primary)/gated.out")
check "and it planned in full, to the same plan" test "$(stat "$E1")|$(plan_of "$E1")" == "$FULL|$(plan_of "$D")"
F=$(backend primary)
check "one consistent verdict is left: the next backend plans from it" test "$(stat "$F")|$(plan_of "$F")" == "$SHARED|$(plan_of "$D")"
exec {HOLD[1]}>&-

# the table is in the registry of the server: the crash restart of a backend that is killed (every backend goes down, the shared memory is made again) discards it
node_conf primary "restart_after_crash = on"
sql primary "SELECT pg_reload_conf()" >/dev/null
sql primary "SELECT pg_sleep(600)" >/dev/null 2>&1 &
until [[ -n $(sql primary "SELECT pid FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()") ]]; do sleep 0.1; done
SIFT_TMP=$(dir primary) leak_expect 1 'terminated by signal 9'
kill -9 "$(sql primary "SELECT pid FROM pg_stat_activity WHERE query LIKE 'SELECT pg_sleep(600)%' AND pid <> pg_backend_pid()")"
wait
until sql primary "SELECT 1" >/dev/null 2>&1; do sleep 0.2; done
G=$(backend primary)
H=$(backend primary)
check "after a crash restart the first backend plans in full" test "$(stat "$G")" == "$FULL"
check "and the next one from it, to the plan from before" test "$(stat "$H")|$(plan_of "$H")" == "$SHARED|$(plan_of "$D")"

node_clone primary standby 3 "hot_standby_feedback = off"
node_start standby || bail "standby start failed"
catchup primary standby
I=$(backend standby)
J=$(backend standby)
check "a hot standby plans in full in its first backend" test "$(stat "$I")" == "$FULL"
check "and the next backend of the standby plans from its verdict" test "$(stat "$J")|$(plan_of "$J")" == "$SHARED|$(plan_of "$D")"
finish
