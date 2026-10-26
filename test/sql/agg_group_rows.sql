-- The rows a SiftAgg path reports to the plan above. A scope list PostgreSQL cannot count (a sub-select with filters on a jsonb path has no
-- statistics: it expects a row or two) makes it expect one group for a scan that makes thousands, and a plan that joins the aggregate to a
-- grouped copy of itself on a null-safe key (not hashable, so a nested loop) then reads the copy again for each row: the aggregate under it runs
-- once per row, spilling each time. The path reports the groups of the rows the lists can pass at the geometric mean of PostgreSQL's count
-- and of every row (within a factor of 64 of the real groups, not of the guess), so the plan above keeps the copy (Materialize) and runs its
-- aggregate once. The estimate is not part of the price: the choice of the scan does not move. A list PostgreSQL has statistics for keeps its count.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 100;
SET random_page_cost = 100;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;

CREATE TABLE gr_facts (id int, parent_id int, kind text, ts date, amount float8) WITH (autovacuum_enabled = false);
INSERT INTO gr_facts SELECT i, i, 'cat_' || chr(97 + i % 4), date '2024-01-01' + i % 365, i % 13 + 0.5 FROM generate_series(1, 12000) i;
CREATE INDEX gr_facts_col ON gr_facts USING sift_col (id, parent_id, kind, ts, amount);
CREATE TABLE gr_dims (id int PRIMARY KEY, settings jsonb NOT NULL, grp int NOT NULL);
INSERT INTO gr_dims SELECT p, jsonb_build_object('kind', 'cat_' || chr(97 + p % 3), 'slot', p % 6), p % 3 FROM generate_series(1, 12000) p;
VACUUM (FREEZE, ANALYZE) gr_facts, gr_dims;

-- the scan's groups against the rows it made, the runs of the aggregate under the join, and the plan above
CREATE FUNCTION shape(q text) RETURNS TABLE (scan text, groups_within_factor boolean, aggregate_runs numeric, plan_above text) LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	n jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS ON, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
	n := jsonb_path_query_first(j, '$.** ? (@."Custom Plan Provider" == "SiftAgg")');
	scan := CASE WHEN n IS NULL THEN 'normal' ELSE 'SiftAgg' END;
	groups_within_factor := (n ->> 'Actual Rows')::numeric BETWEEN (n ->> 'Plan Rows')::numeric / 64 AND 64 * (n ->> 'Plan Rows')::numeric;
	aggregate_runs := (SELECT max((x ->> 'Actual Loops')::numeric) FROM jsonb_path_query(j, '$.** ? (@."Node Type" == "Aggregate")') x);
	plan_above := CASE WHEN j::text LIKE '%"Node Type": "Materialize"%' THEN 'materialized' ELSE 'rescanned' END;
	RETURN NEXT;
END $$;

CREATE FUNCTION scoped_shape(scope text) RETURNS text LANGUAGE sql AS $$ SELECT format($f$
WITH scope AS (SELECT DISTINCT d.id FROM gr_dims d WHERE %s),
g AS (
  SELECT f.parent_id %% 8 AS blk, f.parent_id, f.kind, (f.ts - date '2024-01-01') / 7 AS bucket, sum(f.amount) AS total
  FROM gr_facts f WHERE f.parent_id IN (SELECT id FROM scope) GROUP BY 1, 2, 3, 4
)
SELECT g.parent_id, g.kind, g.bucket, g.total, a.avg_total
FROM g LEFT JOIN (SELECT blk, kind, bucket, avg(total) AS avg_total FROM g GROUP BY blk, kind, bucket) a
  ON NOT (g.blk IS DISTINCT FROM a.blk) AND NOT (g.kind IS DISTINCT FROM a.kind) AND NOT (g.bucket IS DISTINCT FROM a.bucket)$f$, scope) $$;

-- the list of a jsonb path: PostgreSQL expects a row or two of it
SELECT * FROM shape(scoped_shape($$d.settings ->> 'kind' = 'cat_a' AND d.settings ->> 'slot' IN ('0', '3')$$));
-- a list of a column it has statistics for: its count stands
SELECT * FROM shape(scoped_shape($$d.grp = 0$$));
SET sift.enable_agg = off;
SELECT scan, aggregate_runs FROM shape(scoped_shape($$d.grp = 0$$));

-- a filter of the scanned table that PostgreSQL has no statistics of (an expression) keeps a guess of its rows, a few dozen of a large table: the groups of
-- two such scans, FULL JOINed on their keys, are the key columns' own distinct values and the join above is hashed, not nested
CREATE FUNCTION expr_shape(q text) RETURNS TABLE (scan text, groups_within_factor boolean, join_above text) LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
	n jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS ON, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
	n := jsonb_path_query_first(j, '$.** ? (@."Custom Plan Provider" == "SiftAgg")');
	scan := CASE WHEN n IS NULL THEN 'normal' ELSE 'SiftAgg' END;
	groups_within_factor := (n ->> 'Actual Rows')::numeric BETWEEN (n ->> 'Plan Rows')::numeric / 64 AND 64 * (n ->> 'Plan Rows')::numeric;
	join_above := jsonb_path_query_first(j, '$.** ? (@."Node Type" like_regex "Join")."Node Type"') #>> '{}';
	RETURN NEXT;
END $$;
SET sift.enable_agg = on;
SELECT * FROM expr_shape($$
WITH a AS (SELECT parent_id, (ts - date '2024-01-01') / 30 AS bucket, sum(amount) AS va FROM gr_facts WHERE parent_id % 4 = 1 GROUP BY 1, 2),
     b AS (SELECT parent_id, (ts - date '2024-01-01') / 30 AS bucket, sum(amount) AS vb FROM gr_facts WHERE parent_id % 4 = 2 GROUP BY 1, 2)
SELECT a.parent_id, a.bucket, va, vb FROM a FULL JOIN b ON a.parent_id = b.parent_id AND a.bucket = b.bucket$$);
DROP FUNCTION expr_shape(text);

-- the lift is bounded: the groups of a lookup list PostgreSQL cannot count are reported up to the plan's own plus a thousand, where more only
-- multiplies in the join above (two scans of a large table, their rows times a default join selectivity, priced the plan that keeps one of them normal)
SET sift.plan_scale = 1000;
CREATE FUNCTION lifted_rows(q text) RETURNS TABLE (scan_rows_within_bound boolean, join_rows_within_bound boolean) LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (COSTS ON, FORMAT JSON) ' || q INTO j;
	scan_rows_within_bound := (SELECT bool_and((x ->> 'Plan Rows')::numeric <= 2048) FROM jsonb_path_query(j, '$.** ? (@."Custom Plan Provider" == "SiftAgg")') x);
	join_rows_within_bound := (SELECT bool_and((x ->> 'Plan Rows')::numeric <= 20000) FROM jsonb_path_query(j, '$.** ? (@."Node Type" like_regex "Join")') x);
	RETURN NEXT;
END $$;
SELECT * FROM lifted_rows($$
WITH scope AS (SELECT DISTINCT d.id FROM gr_dims d WHERE d.settings ->> 'kind' = 'cat_a' AND d.settings ->> 'slot' IN ('0', '3')),
a AS (SELECT f.parent_id % 8 AS blk, f.kind, (f.ts - date '2024-01-01') / 7 AS bucket, sum(f.amount) AS va FROM gr_facts f WHERE f.parent_id IN (SELECT id FROM scope) GROUP BY 1, 2, 3),
b AS (SELECT f.parent_id % 8 AS blk, f.kind, (f.ts - date '2024-01-01') / 7 AS bucket, avg(f.amount) AS vb FROM gr_facts f WHERE f.parent_id IN (SELECT id FROM scope) GROUP BY 1, 2, 3)
SELECT a.blk, va, vb FROM a FULL JOIN b ON jsonb_build_array(a.blk, a.kind, a.bucket) = jsonb_build_array(b.blk, b.kind, b.bucket)$$);
RESET sift.plan_scale;
DROP FUNCTION lifted_rows(text);

DROP FUNCTION shape(text), scoped_shape(text);
DROP TABLE gr_facts, gr_dims;
DROP EXTENSION pg_sift;
