-- The groups the planner expects of an unnest over a lookup it has no statistics for (a CTE of a function's rows, which PostgreSQL takes as the one it was told): PostgreSQL's rows are
-- the floor it gives the lookup (one) times the 100 of the function, a handful of rows where the elements of the table's rows are many and the groups
-- are as many as the distinct values of the key. The scan reports the groups of the distinct values the index holds for the key, no more than the
-- elements: within a factor of two of the groups it returns, where PostgreSQL's rows are a tenth of them.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

CREATE TABLE ug_rows (id int PRIMARY KEY, prop_id int NOT NULL, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ug_rows SELECT i, 1 + i % 40,
	jsonb_build_array(jsonb_build_object('name', 'n' || (hashint4(i) & 8191)), jsonb_build_object('name', 'n' || (hashint4(i + 7) & 8191)),
					  jsonb_build_object('name', 'n' || (hashint4(i + 13) & 8191)))
	FROM generate_series(1, 60000) i;
SET client_min_messages = warning;
CREATE INDEX ug_rows_col ON ug_rows USING sift_col (id, prop_id, items);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) ug_rows;
-- a lookup whose rows PostgreSQL takes as one, whatever it returns (the ids of every row: the join keeps them all)
CREATE FUNCTION ug_ids() RETURNS SETOF int LANGUAGE sql STABLE ROWS 1 AS 'SELECT id FROM ug_rows';

CREATE FUNCTION est_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN substring(l FROM 'rows=(\d+)')::bigint; END IF;
	END LOOP;
	RETURN NULL;
END $$;

CREATE FUNCTION groups_of(q text) RETURNS TABLE (planned_rows_ok bool, groups bigint) LANGUAGE plpgsql AS $$
DECLARE e bigint;
BEGIN
	e := est_rows(q);
	EXECUTE format('SELECT count(*) FROM (%s) x', q) INTO groups;
	planned_rows_ok := e IS NOT NULL AND e BETWEEN groups / 2.0 AND groups * 2.0;
	RETURN NEXT;
END $$;

SELECT planned_rows_ok, groups > 5000 AS many FROM groups_of($q$
WITH ids AS MATERIALIZED (SELECT ug_ids() AS id)
SELECT e->>'name' AS name, count(*) AS n FROM ug_rows r JOIN ids ON ids.id = r.id, jsonb_array_elements(r.items) e GROUP BY 1
$q$);

DROP FUNCTION groups_of(text), est_rows(text), ug_ids();
DROP TABLE ug_rows;
DROP EXTENSION pg_sift;
