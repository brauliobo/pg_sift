-- Custom Scan (SiftAgg) over the test column source: the plan is taken for the shapes it proves, and its rows are the ones of the
-- normal plan (sift.enable_agg off) for aggregates, group keys, filters, NULLs and unnested arrays of objects.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0;

CREATE TABLE o (id int PRIMARY KEY, store int, placed date, kind text, amount float8, qty int, note text, items jsonb, pad text)
	WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o
SELECT g, g % 7, date '2024-01-01' + g % 400, (ARRAY['web', 'shop', 'phone'])[1 + g % 3], CASE WHEN g % 11 = 0 THEN NULL ELSE (g % 100) * 1.5 END,
	   CASE WHEN g % 13 = 0 THEN NULL ELSE g % 17 END, CASE WHEN g % 5 = 0 THEN NULL ELSE 'n' || g % 9 END,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE jsonb_build_array(jsonb_build_object('category', (ARRAY['A', 'B', 'C', 'D'])[1 + g % 4], 'qty', g % 5, 'price', (g % 9) * 2.5),
															 jsonb_build_object('category', (ARRAY['A', 'B', 'C', 'D'])[1 + (g / 3) % 4], 'qty', g % 3, 'price', (g % 7) * 1.5)) END,
	   repeat('x', 100)
FROM generate_series(1, 6000) g;
CREATE INDEX o_col ON o USING sift_colstub (id, store, placed, kind, amount, qty, note, items) WITH (paths = 'items: []{category text, qty int4, price float8}');
VACUUM ANALYZE o;

-- the plan of a row level and of an unnesting query
EXPLAIN (COSTS OFF) SELECT kind, count(*), sum(amount) FROM o WHERE store IN (1, 2, 3) GROUP BY kind;
EXPLAIN (COSTS OFF) SELECT i.category, count(*), avg(i.price)
FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) WHERE i.qty > 1 GROUP BY 1;

-- shapes it does not take keep the normal plan
EXPLAIN (COSTS OFF) SELECT kind, sum(amount::float4) FROM o GROUP BY kind;
-- a range of a column that follows the heap order is priced by the units its zone leaves, and is cheaper than the btree scan
EXPLAIN (COSTS OFF) SELECT kind, count(*) FROM o WHERE id < 100 GROUP BY kind;

-- the rows of the two plans
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND b > 0;
END $$;
SELECT same($$SELECT kind, count(*) AS n, count(amount) AS c, sum(amount) AS s, avg(amount) AS a, min(amount) AS lo, max(amount) AS hi FROM o GROUP BY kind$$) AS floats;
SELECT same($$SELECT store, count(*) AS n, sum(qty) AS s, avg(qty) AS a, min(placed) AS lo, max(placed) AS hi FROM o WHERE kind <> 'web' AND placed >= DATE '2024-03-01' GROUP BY store$$) AS ints;
SELECT same($$SELECT note, count(*) AS n, count(DISTINCT kind) AS dk, count(DISTINCT qty) AS dq FROM o GROUP BY note$$) AS distincts;
SELECT same($$SELECT date_trunc('month', placed)::date AS m, lower(kind) AS k, count(*) FILTER (WHERE amount > 50) AS big, count(*) FILTER (WHERE qty IS NULL) AS noqty
			  FROM o WHERE store NOT IN (0, 6) GROUP BY 1, 2$$) AS expressions;
SELECT same($$SELECT i.category, count(*) AS n, avg(i.price) AS a, sum(i.price) AS s, count(DISTINCT o.id) AS orders, sum(i.qty * (i.price - 1)) AS rev
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  WHERE o.store < 5 AND i.price > 2 GROUP BY 1$$) AS unnest;
SELECT same($$SELECT count(*) AS n, sum(qty) AS s, avg(amount) AS a FROM o WHERE store = 99 OR store IS NULL$$) AS empty;

-- a delete and an update after the index: the scan sees the snapshot's rows
DELETE FROM o WHERE id % 10 = 0;
UPDATE o SET kind = 'web', amount = amount + 1 WHERE id % 7 = 1;
SELECT same($$SELECT kind, count(*) AS n, sum(amount) AS s FROM o GROUP BY kind$$) AS after_dml;

-- integers spread over the whole int4 range: group keys and IN lists that no table can cover
CREATE TABLE w AS SELECT g AS id, ((g::bigint * 1000003) % 2000000011)::int AS a, g % 5 AS b, CASE WHEN g % 7 = 0 THEN NULL ELSE g % 3 END AS c FROM generate_series(1, 5000) g;
CREATE INDEX w_col ON w USING sift_colstub (id, a, b, c);
VACUUM ANALYZE w;
EXPLAIN (COSTS OFF) SELECT a, count(*) FROM w GROUP BY a;
SELECT same($$SELECT a, count(*) AS n, sum(b) AS s FROM w GROUP BY a$$) AS wide_key;
SELECT same($$SELECT a, c, count(*) AS n FROM w GROUP BY a, c$$) AS wide_key_and_null;
SELECT same($$SELECT b, count(*) AS n FROM w WHERE a IN (1000003, 2000006, 3000009, 1999999000, 5, -2000000000) GROUP BY b$$) AS wide_in;
SELECT same($$SELECT b, count(*) AS n FROM w WHERE a NOT IN (1000003, 2000006, 3000009, 1999999000, 5, -2000000000) GROUP BY b$$) AS wide_not_in;

-- ordered-set aggregates: the plan, and the rows of the normal plan with the histogram and, at the smallest work_mem, with the sort
EXPLAIN (COSTS OFF) SELECT kind, percentile_cont(0.5) WITHIN GROUP (ORDER BY amount) FROM o GROUP BY kind;
EXPLAIN (COSTS OFF) SELECT kind, percentile_cont(ARRAY[0.5]) WITHIN GROUP (ORDER BY amount) FROM o GROUP BY kind;
SELECT same($$SELECT kind, percentile_cont(0.5) WITHIN GROUP (ORDER BY amount) AS c, percentile_cont(0.9) WITHIN GROUP (ORDER BY amount DESC) AS cd,
			  percentile_disc(0.3) WITHIN GROUP (ORDER BY qty) AS dq, percentile_disc(0.7) WITHIN GROUP (ORDER BY placed DESC) AS dp FROM o GROUP BY kind$$) AS percentiles;
SET work_mem = '64kB';
SELECT same($$SELECT i.category, percentile_cont(0.5) WITHIN GROUP (ORDER BY i.price * 1.1 + o.id) AS c, percentile_disc(0.05) WITHIN GROUP (ORDER BY i.price * 1.1 + o.id) AS d
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY 1$$) AS spilled;
RESET work_mem;

-- a CTE that several arms read is planned inline when the arms run on the index; the rows are those of the materialized CTE
EXPLAIN (COSTS OFF) WITH b AS (SELECT o.id, o.placed, i.qty, i.qty * i.price AS net FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int, price float8) WHERE o.store < 6)
			  SELECT 'day' AS g, placed AS d, count(DISTINCT id) AS n, sum(qty) AS q, sum(net) AS r FROM b GROUP BY 2
			  UNION ALL SELECT 'month', date_trunc('month', placed)::date, count(DISTINCT id), sum(qty), sum(net) FROM b GROUP BY 2;
SELECT same($$WITH b AS (SELECT o.id, o.placed, i.qty, i.qty * i.price AS net FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int, price float8) WHERE o.store < 6)
			  SELECT 'day' AS g, placed AS d, count(DISTINCT id) AS n, sum(qty) AS q, sum(net) AS r FROM b GROUP BY 2
			  UNION ALL SELECT 'month', date_trunc('month', placed)::date, count(DISTINCT id), sum(qty), sum(net) FROM b GROUP BY 2$$) AS shared_cte;
