-- The rows of the scan of a CTE are lifted toward the rows its body reads from a table with a column index (agg_cte_floor), because the joins in
-- the body leave a row or two where the data keeps most of them. A body that aggregates, limits or combines sets does not return what it reads:
-- its rows are its own estimate, one for each group, and the lift is not made for it.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE kinds (id int PRIMARY KEY, label text);
INSERT INTO kinds SELECT g, 'kind ' || g FROM generate_series(1, 20) g;
CREATE TABLE events (id int PRIMARY KEY, kind_id int NOT NULL, ts date NOT NULL, amount int);
INSERT INTO events SELECT g, 1 + g % 20, date '2025-01-01' + (g * 7919 % 360), g % 100 FROM generate_series(1, 20000) g;
CREATE INDEX events_col ON events USING sift_col (id, kind_id, ts, amount);
VACUUM (FREEZE, ANALYZE) events;
ANALYZE kinds;

-- the rows PostgreSQL expects of the scan of CTE g in the plan
CREATE FUNCTION cte_rows(body text) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE format('EXPLAIN (FORMAT JSON) WITH g AS MATERIALIZED (%s) SELECT k.id, g.n FROM g JOIN kinds k ON k.id = g.kind_id', body) INTO j;
	RETURN (jsonb_path_query_first(j, '$.** ? (@."Node Type" == "CTE Scan" && @."CTE Name" == "g")') ->> 'Plan Rows')::float8;
END $$;

-- 20 groups out of 20,000 rows: the estimate of the groups stands
SELECT cte_rows($$SELECT kind_id, count(*) AS n FROM events WHERE ts >= date '2025-03-01' GROUP BY kind_id$$) < 40 AS groups_not_lifted;
-- a limit: the estimate of the limit stands
SELECT cte_rows($$SELECT kind_id, count(*) AS n FROM (SELECT * FROM events WHERE ts >= date '2025-03-01' LIMIT 7) l GROUP BY kind_id$$) < 40 AS limit_not_lifted;
-- a body that keeps its rows is lifted toward them, as agg_cte_floor has it
SELECT cte_rows($$SELECT kind_id, amount AS n FROM events WHERE amount IS NOT NULL AND ts >= date '2025-03-01'$$) > 40 AS rows_lifted;
DROP FUNCTION cte_rows;
DROP TABLE events, kinds;
DROP EXTENSION pg_sift;
