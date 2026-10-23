#!/usr/bin/env bash
# The first statement of a session that has not preloaded the library loads it while the planner opens the column index, after planner() went
# without sift_planner: its CTE is not planned inline and the aggregates over it run the ordinary plan. The executor start plans that statement
# again through the hook (agg/cte.c), so it runs in the scan like the statements after it. A fresh psql is a fresh backend: nothing
# is loaded before the statement under test, and sift.stats() reads the scans that backend ran.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

plan 8
node_init af 27 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start af || bail "start failed"
sql af "CREATE EXTENSION pg_sift" \
	"CREATE TABLE entities (id int PRIMARY KEY, kind text)" \
	"INSERT INTO entities SELECT i, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4] FROM generate_series(1, 60) i" \
	"CREATE TABLE facts (id int, day date, entity_id int, deleted_at timestamp, items jsonb)" \
	"INSERT INTO facts SELECT i, '2026-01-01'::date + (i % 15), 1 + i % 70, CASE WHEN i % 17 = 0 THEN now() END,
		jsonb_build_array(jsonb_build_object('qty', i % 5), jsonb_build_object('qty', i % 3)) FROM generate_series(1, 60000) i" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, day, entity_id, deleted_at, items) WITH (paths = 'items[]{qty float8}')" \
	"VACUUM (FREEZE, ANALYZE) facts" "ANALYZE entities" >/dev/null || bail "setup failed"

# The arms of a UNION ALL read one CTE: the inline plan runs them in the scan, the shared CTE does not
Q="SELECT * FROM (
  WITH scoped AS (SELECT f.id, f.day, e.kind, i.qty FROM facts f JOIN entities e ON e.id = f.entity_id CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS i(qty float8) WHERE f.deleted_at IS NULL)
  SELECT 'day' AS grp, day::text AS k, count(DISTINCT id) AS n, sum(qty) AS q FROM scoped GROUP BY day
  UNION ALL
  SELECT 'kind', kind, count(DISTINCT id), sum(qty) FROM scoped GROUP BY kind
) q ORDER BY 1, 2"

# session STATEMENT...: one fresh backend runs the statements; the last two lines are the scans it ran and the md5 of the first statement's rows
session() {
	{ for s in "$@"; do echo "$s;"; done; echo "SELECT 'scans', value FROM sift.stats() WHERE name = 'scans';"; } | sql af
}
scans() { sed -n 's/^scans|//p' <<<"$1"; }

on=$(session "$Q")
check "the first statement of a session runs in the scan" test "$(scans "$on")" -ge 1
off=$(session "SET sift.enable_agg = off" "$Q")
check "with the pushdown off it does not" test "$(scans "$off")" == 0
check "the rows are those of the normal plan" test "$(grep -v '^scans|' <<<"$on" | "$REPO/scripts/sha256.sh")" == "$(grep -v '^scans|' <<<"$off" | "$REPO/scripts/sha256.sh")"

two=$(session "$Q" "$Q")
check "the second statement runs in the scan too" test "$(scans "$two")" -ge 2
pre=$(session "PREPARE q AS $Q" "EXECUTE q" "EXECUTE q" "EXECUTE q")
check "a prepared statement is not left with the plan made without the hook" test "$(scans "$pre")" -ge 3
multi=$(session "SELECT 1" "$Q")
check "a statement after one that did not touch the index is planned the same" test "$(scans "$multi")" -ge 1
param=$(session "PREPARE p(int) AS SELECT count(*) FROM facts WHERE entity_id = \$1" "EXECUTE p(3)" "SELECT count(*) FROM facts WHERE entity_id = 3")
check "a statement with parameters keeps its plan and its rows" test "$(grep -c '^[0-9]' <<<"$param")" == 2
check "the server log shows no error" test "$(grep -c -E 'ERROR|FATAL|PANIC' "$(dir af)/postgres.log")" == 0

finish
