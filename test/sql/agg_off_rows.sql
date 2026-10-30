-- sift.enable_agg = off leaves the normal plan as PostgreSQL makes it. The rows of a CTE scan and of a sub-select scan that the aggregate scan's choice is
-- priced from (a floor over a default guess of a row) are lifted for that choice only; with the scan off they moved the joins of the normal plan. The plan
-- that stays when the scans of a shared CTE lose has PostgreSQL's rows of the CTE too (agg_cte_stock_rows); the sub-select is lifted in the plan it has.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
\set rows 20000
\ir test/agg_cte_inline_fixture.sql
-- the rows the plan expects of the scan of a CTE that two sub-selects read, and of a sub-select whose filters have no statistics
CREATE FUNCTION scan_rows(q text, node text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j;
	RETURN (jsonb_path_query_first(j, format('$.** ? (@."Node Type" == "%s")', node)::jsonpath) ->> 'Plan Rows')::int;
END $$;
-- the sub-select has the kind of the join: PostgreSQL 19 merges a join of a table with itself on its key and then proves two kinds empty, a CTE of no rows
CREATE FUNCTION cte_query() RETURNS text LANGUAGE sql AS $$
	SELECT $q$WITH c AS MATERIALIZED (SELECT e.id, e.payload FROM events e JOIN dims d ON d.id = e.parent_id WHERE d.kind = 'kind_a' AND d.id IN (SELECT id FROM dims WHERE kind = 'kind_a' AND id < 0))
	SELECT (SELECT count(*) FROM c), (SELECT max(id) FROM c)$q$ $$;
CREATE FUNCTION sub_query() RETURNS text LANGUAGE sql AS $$
	SELECT $q$SELECT count(*) FROM (SELECT id, payload ->> 'a' AS a, payload ->> 'b' AS b FROM events ORDER BY id LIMIT 10000) s WHERE s.a = 'x' AND s.b = 'y'$q$ $$;
SELECT scan_rows(cte_query(), 'CTE Scan') = 1 AS cte_stock, scan_rows(sub_query(), 'Subquery Scan') > 1 AS sub_lifted;
SET sift.enable_agg = off;
SELECT scan_rows(cte_query(), 'CTE Scan') AS cte_rows, scan_rows(sub_query(), 'Subquery Scan') AS sub_rows;
RESET sift.enable_agg;
DROP FUNCTION scan_rows, cte_query, sub_query, series_query, differences;
DROP TABLE events, dims, maps;
DROP EXTENSION pg_sift;
