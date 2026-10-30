-- count(DISTINCT id) of a unique id over the elements of an array of the row counts each row once in every group, so the group of a coarser grouping set (a ROLLUP's
-- total) is not the sum of the finer groups: a row with two elements of different kinds is in two of them and in the total once.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE su_orders (id int PRIMARY KEY, tenant_id int NOT NULL, items jsonb NOT NULL);
INSERT INTO su_orders SELECT g, 1 + g % 7,
	jsonb_build_array(jsonb_build_object('kind', 'k' || g % 4, 'qty', 1 + g % 5), jsonb_build_object('kind', 'k' || (g + 1) % 4, 'qty', 2))
FROM generate_series(1, 20000) g;
CREATE INDEX su_orders_col ON su_orders USING sift_col (id, tenant_id, items) WITH (paths = 'items[]{kind text, qty int}');
VACUUM (FREEZE, ANALYZE) su_orders;

CREATE FUNCTION su_q() RETURNS text LANGUAGE sql AS $$ SELECT $q$SELECT i.kind, grouping(i.kind) AS g_kind, count(*) AS n, count(DISTINCT o.id) AS orders
FROM su_orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(kind text, qty int) WHERE o.tenant_id < 6 GROUP BY ROLLUP (i.kind)$q$ $$;

CREATE FUNCTION su_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

SET sift.agg_force = on;
SELECT su_scan(su_q()) AS scan;
CREATE TEMP TABLE su_on AS SELECT * FROM (SELECT i.kind, grouping(i.kind) AS g_kind, count(*) AS n, count(DISTINCT o.id) AS orders
	FROM su_orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(kind text, qty int) WHERE o.tenant_id < 6 GROUP BY ROLLUP (i.kind)) x;
RESET sift.agg_force;
SET sift.enable_agg = off;
CREATE TEMP TABLE su_off AS SELECT * FROM (SELECT i.kind, grouping(i.kind) AS g_kind, count(*) AS n, count(DISTINCT o.id) AS orders
	FROM su_orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(kind text, qty int) WHERE o.tenant_id < 6 GROUP BY ROLLUP (i.kind)) x;
RESET sift.enable_agg;
SELECT (SELECT count(*) FROM (TABLE su_on EXCEPT TABLE su_off) d) + (SELECT count(*) FROM (TABLE su_off EXCEPT TABLE su_on) d) AS rows_unlike_normal;
SELECT orders AS total_orders FROM su_on WHERE g_kind = 1;

DROP FUNCTION su_scan(text), su_q();
DROP TABLE su_orders;
DROP EXTENSION pg_sift;
