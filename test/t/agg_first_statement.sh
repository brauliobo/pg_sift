#!/usr/bin/env bash
# The first statement of a session that has not preloaded the library loads it while the planner opens the column index, after planner() went
# without sift_planner: its CTE is not planned inline and the aggregates over it run the ordinary plan. The executor start plans that statement
# again through the hook (agg/cte.c), so it runs in the scan like the statements after it. A fresh psql is a fresh backend: nothing
# is loaded before the statement under test.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

plan 8
# the plans are read from auto_explain's log: a server built without the contrib modules (PostgreSQL from git in CI) has none
[[ -e $(pg_config --pkglibdir)/auto_explain.so ]] || { skip 8 "auto_explain is not installed"; exit 0; }
node_init af 27 "autovacuum = off
max_parallel_workers_per_gather = 0"
node_start af || bail "start failed"
sql af "CREATE EXTENSION pg_sift" \
	"CREATE TABLE dim_a (id int PRIMARY KEY, kind text)" \
	"INSERT INTO dim_a SELECT i, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4] FROM generate_series(1, 60) i" \
	"CREATE TABLE facts (id int, day date, dim_a_id int, deleted_at timestamp, items jsonb)" \
	"INSERT INTO facts SELECT i, '2026-01-01'::date + (i % 15), 1 + i % 70, CASE WHEN i % 17 = 0 THEN now() END,
		jsonb_build_array(jsonb_build_object('qty', i % 5), jsonb_build_object('qty', i % 3)) FROM generate_series(1, 60000) i" \
	"CREATE INDEX facts_col ON facts USING sift_col (id, day, dim_a_id, deleted_at, items) WITH (paths = 'items[]{qty float8}')" \
	"VACUUM (FREEZE, ANALYZE) facts" "ANALYZE dim_a" >/dev/null || bail "setup failed"

# The arms of a UNION ALL read one CTE: the inline plan runs them in the scan, the shared CTE does not
Q="SELECT * FROM (
  WITH scoped AS (SELECT f.id, f.day, e.kind, i.qty FROM facts f JOIN dim_a e ON e.id = f.dim_a_id CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS i(qty float8) WHERE f.deleted_at IS NULL)
  SELECT 'day' AS grp, day::text AS k, count(DISTINCT id) AS n, sum(qty) AS q FROM scoped GROUP BY day
  UNION ALL
  SELECT 'kind', kind, count(DISTINCT id), sum(qty) FROM scoped GROUP BY kind
) q ORDER BY 1, 2"

# session STATEMENT...: one fresh backend runs the statements with auto_explain logging every plan it executes, and nodes is the number of SiftAgg nodes
# in the plans that session logged (3.2 has no scan counters; EXPLAIN ANALYZE would hide the case, its statement is not a SELECT to plan again)
log=$(dir af)/postgres.log
session() {
	{ echo "LOAD 'auto_explain';"; echo "SET auto_explain.log_min_duration = 0;"; for s in "$@"; do echo "$s;"; done; } | sql af >/dev/null
}
nodes() {
	local off
	off=$(stat -c %s "$log")
	session "$@"
	tail -c +$((off + 1)) "$log" | grep -c 'Custom Scan (SiftAgg)' || true
}
rows() { { for s in "$@"; do echo "$s;"; done; } | sql af | grep -v '^SET$' | "$REPO/scripts/sha256.sh"; }

check "the first statement of a session runs in the scan" test "$(nodes "$Q")" -ge 1
check "with the pushdown off it does not" test "$(nodes "SET sift.enable_agg = off" "$Q")" == 0
check "the rows are those of the normal plan" test "$(rows "$Q")" == "$(rows "SET sift.enable_agg = off" "$Q")"

check "the second statement runs in the scan too" test "$(nodes "$Q" "$Q")" -ge 2
check "a prepared statement is not left with the plan made without the hook" test "$(nodes "PREPARE q AS $Q" "EXECUTE q" "EXECUTE q" "EXECUTE q")" -ge 3
check "a statement after one that did not touch the index is planned the same" test "$(nodes "SELECT 1" "$Q")" -ge 1
param=$({ echo "PREPARE p(int) AS SELECT count(*) FROM facts WHERE dim_a_id = \$1;"; echo "EXECUTE p(3);"; echo "SELECT count(*) FROM facts WHERE dim_a_id = 3;"; } | sql af)
check "a statement with parameters keeps its plan and its rows" test "$(grep -c '^[0-9]' <<<"$param")" == 2
check "the server log shows no error" test "$(grep -c -E 'ERROR|FATAL|PANIC' "$log")" == 0

finish
