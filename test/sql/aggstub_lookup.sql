-- Custom Scan (SiftAgg) over the test column source: lookup joins to small tables (duplicate, NULL and missing keys, a table that changes),
-- keys and filters over several leaves, a key of an object leaf read at the row level, and an outer aggregate over per-order aggregates.
-- The rows are the ones of the normal plan (sift.enable_agg off).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0;

CREATE TABLE o (id int PRIMARY KEY, store int, kind text, metrics jsonb, items jsonb, pad text) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o
SELECT g, g % 7, (ARRAY['web', 'shop', 'phone'])[1 + g % 3],
	   CASE g % 6 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN jsonb_build_object('value', g % 5) ELSE jsonb_build_object('mhead', 1 + (g % 40) / 10.0, 'value', g % 5) END,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE jsonb_build_array(
			jsonb_build_object('category', (ARRAY['A', 'b', 'C', 'D', 'E'])[1 + g % 5], 'kind', CASE WHEN g % 4 = 0 THEN NULL ELSE (ARRAY['x', 'y'])[1 + g % 2] END, 'qty', g % 5, 'price', (g % 9) * 2.5),
			jsonb_build_object('category', (ARRAY['A', 'b', 'C', 'D', 'E'])[1 + (g / 3) % 5], 'kind', 'y', 'qty', g % 3, 'price', (g % 7) * 1.5)) END,
	   repeat('x', 100)
FROM generate_series(1, 1500) g;
CREATE INDEX o_col ON o USING sift_colstub (id, store, kind, metrics, items) WITH (paths = 'metrics: *:float8; items: []{category text, kind text, qty int4, price float8}');
VACUUM ANALYZE o;

-- the mapping tables: a key with two rows, a NULL key, a key that differs by case, keys without a row (E, x)
CREATE TABLE cat_map (category text, flag_a text);
INSERT INTO cat_map VALUES ('A', 'a1'), ('A', 'a2'), ('B', 'bb'), ('b', 'lower'), ('C', NULL), (NULL, 'nothing'), ('D', 'dd');
CREATE TABLE empty_map (category text, flag_a text);

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	DROP TABLE r_on, r_off;
	RETURN a = 0;
END $$;

-- the plans
EXPLAIN (COSTS OFF) SELECT m.flag_a, count(*) FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
LEFT JOIN cat_map m ON m.category = i.category GROUP BY 1;
EXPLAIN (COSTS OFF) SELECT i.category, avg((o.metrics->>'mhead')::float8) FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY 1;

-- left and inner joins: the duplicate key replicates, the NULL key matches nothing, the case differs, the missing keys are NULL
SELECT same($$SELECT m.flag_a, count(*) AS n, sum(i.qty) AS q, count(DISTINCT o.id) AS orders
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN cat_map m ON m.category = i.category GROUP BY 1$$) AS left_join;
SELECT same($$SELECT i.category, m.flag_a, count(*) AS n, sum(i.price) AS p
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  JOIN cat_map m ON m.category = i.category GROUP BY 1, 2$$) AS inner_join;
SELECT same($$SELECT m.flag_a, count(*) AS n
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
			  LEFT JOIN cat_map m ON m.category = i.kind GROUP BY 1$$) AS null_keys_of_the_data;
SELECT same($$SELECT m.flag_a, count(*) AS n, count(DISTINCT o.id) AS orders
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN empty_map m ON m.category = i.category GROUP BY 1$$) AS empty_left;
SELECT same($$SELECT count(*) AS n, sum(i.qty) AS q
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  JOIN empty_map m ON m.category = i.category$$) AS empty_inner;
SELECT same($$SELECT m.flag_a, count(*) AS n FROM o LEFT JOIN cat_map m ON m.category = o.kind GROUP BY 1$$) AS row_level;

-- keys and filters over several leaves
SELECT same($$SELECT CASE WHEN o.kind = 'web' THEN i.category ELSE coalesce(i.kind, '?') END AS pick, coalesce(o.kind, '') || '/' || (o.store % 2) AS k, count(*) AS n
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
			  WHERE o.kind = 'web' OR i.price > 10 GROUP BY 1, 2$$) AS several_leaves;

-- a key of the metrics next to the unnest
SELECT same($$SELECT i.category, count((o.metrics->>'mhead')::float8) AS c, avg((o.metrics->>'mhead')::float8) AS a, max((o.metrics->>'value')::float8) AS v
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY 1$$) AS metric_key;
SELECT same($$SELECT store, count(*) FILTER (WHERE (metrics->>'value')::float8 >= 2) AS good, avg((metrics->>'mhead')::float8) AS a
			  FROM o WHERE (metrics->>'mhead')::float8 IS NOT NULL GROUP BY 1$$) AS metric_key_rows;

-- the outer aggregate over the per-order aggregates (the order is the group; the plan is the planner's choice at this size, the
-- fixtures of scripts/coldiff.sh check it is taken), and a group that is not one order's share
SELECT same($$SELECT category, avg(p) AS a, count(DISTINCT id) AS orders, sum(n) AS items, count(DISTINCT id) FILTER (WHERE p > 5) AS high
			  FROM (SELECT o.id, i.category, avg(i.price) AS p, count(*) AS n
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY o.id, i.category) x GROUP BY category$$) AS per_order;
SELECT same($$SELECT category, avg(p) AS a, count(*) AS n
			  FROM (SELECT i.category, i.kind, avg(i.price) AS p
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8) GROUP BY 1, 2) x GROUP BY category$$) AS not_per_order;

-- the mapping table changes: the next scan reads the new rows
UPDATE cat_map SET flag_a = 'changed' WHERE category = 'A';
DELETE FROM cat_map WHERE category = 'D';
INSERT INTO empty_map VALUES ('A', 'late'), (NULL, 'late');
SELECT same($$SELECT m.flag_a, count(*) AS n
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN cat_map m ON m.category = i.category GROUP BY 1$$) AS changed_map;
SELECT same($$SELECT m.flag_a, count(*) AS n
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN empty_map m ON m.category = i.category GROUP BY 1$$) AS changed_empty;
