-- Custom Scan (SiftAgg) over the test column source: grouping sets (GROUPING(), the empty set, NULL data against rollup NULLs), a WHERE on
-- the outer query of per-order aggregates, aggregates of the columns of a lookup join and float group keys (-0, 0 and NaN: the stub
-- keeps the values of the heap). The rows are the ones of the normal plan (sift.enable_agg off).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0;
-- the tables are tiny: the scan's start-up cost would keep the btree plans these tests are not about
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE o (id int PRIMARY KEY, store int, kind text, f8 float8, f4 float4, items jsonb, pad text) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o
SELECT g, g % 7, CASE WHEN g % 5 = 0 THEN NULL ELSE (ARRAY['web', 'shop', 'phone'])[1 + g % 3] END,
	   (ARRAY['0', '-0', 'NaN', '1.5', 'Infinity', NULL, '1.5', '-Infinity'])[1 + g % 8]::float8, (ARRAY['0', '-0', 'NaN', '2.5', NULL])[1 + g % 5]::float4,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE jsonb_build_array(
			jsonb_build_object('category', (ARRAY['A', 'b', 'C', 'D', 'E'])[1 + g % 5], 'kind', CASE WHEN g % 4 = 0 THEN NULL ELSE (ARRAY['x', 'y'])[1 + g % 2] END, 'qty', g % 5, 'price', (g % 9) * 2.5),
			jsonb_build_object('category', (ARRAY['A', 'b', 'C', 'D', 'E'])[1 + (g / 3) % 5], 'kind', 'y', 'qty', g % 3, 'price', (g % 7) * 1.5)) END,
	   repeat('x', 100)
FROM generate_series(1, 1500) g;
CREATE INDEX o_col ON o USING sift_colstub (id, store, kind, f8, f4, items) WITH (paths = 'items: []{category text, kind text, qty int4, price float8}');
VACUUM ANALYZE o;

-- the values to aggregate through a lookup join: a key with two rows, NULL values, a key without a row (E)
CREATE TABLE vals (category text, label text, weight numeric(8,2), factor float8);
INSERT INTO vals VALUES ('A', 'a1', 1.50, 0.5), ('A', 'a2', 2.25, NULL), ('b', NULL, 3.10, 2.0), ('C', 'cc', NULL, 1.5), ('D', 'dd', 0.05, 0.25);

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
EXPLAIN (COSTS OFF) SELECT kind, store, grouping(kind, store) AS g, count(*) FROM o GROUP BY ROLLUP (kind, store);
EXPLAIN (COSTS OFF) SELECT f8, count(*) FROM o GROUP BY f8;
EXPLAIN (COSTS OFF) SELECT v.label, sum(v.weight), max(v.label), count(DISTINCT v.factor)
FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) LEFT JOIN vals v ON v.category = i.category GROUP BY 1;

-- grouping sets: the NULLs of the data and of the rollup, GROUPING() of a subset, a HAVING, the empty set over no row, a set twice, GROUP BY DISTINCT
SELECT same($$SELECT kind, store, grouping(kind, store) AS g, grouping(store) AS gs, count(*) AS n, sum(store) AS s FROM o GROUP BY ROLLUP (kind, store)$$) AS rollup_nulls;
SELECT same($$SELECT kind, store % 2 AS odd, grouping(store % 2, kind) AS g, count(*) AS n, count(DISTINCT id) AS ids
			  FROM o GROUP BY CUBE (kind, store % 2) HAVING grouping(kind) = 0 OR count(*) > 300$$) AS cube_having;
SELECT same($$SELECT kind, grouping(kind) AS g, count(*) AS n, sum(store) AS s, min(id) AS m FROM o WHERE store < 0 GROUP BY GROUPING SETS ((kind), ())$$) AS empty_set;
SELECT same($$SELECT kind, count(*) AS n FROM o GROUP BY GROUPING SETS ((kind), (kind), ())$$) AS set_twice;
SELECT same($$SELECT kind, store, count(*) AS n FROM o GROUP BY DISTINCT ROLLUP (kind, store), ROLLUP (kind)$$) AS group_distinct;
SELECT same($$SELECT i.category, i.kind, grouping(i.category, i.kind) AS g, count(*) AS n, avg(i.price) AS a
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
			  GROUP BY GROUPING SETS ((i.category, i.kind), (i.category), ())$$) AS unnest_sets;

-- float keys: -0 and 0 are one group, every NaN is one group, NULL and the infinities are their own
SELECT same($$SELECT f8, count(*) AS n, sum(id) AS s FROM o GROUP BY f8$$) AS float8_key;
SELECT same($$SELECT f4, f8, count(*) AS n FROM o GROUP BY f4, f8$$) AS float4_float8_keys;
SELECT same($$SELECT f8, kind, count(*) AS n FROM o GROUP BY ROLLUP (f8, kind)$$) AS float_key_sets;
SELECT same($$SELECT i.price, count(*) AS n, count(DISTINCT o.id) AS orders
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY i.price$$) AS float_key_items;
-- the groups: one for 0 and -0 (the heap has both), one for the NaNs
SELECT CASE WHEN f8 IS NULL THEN 'null' WHEN f8 = 'NaN' THEN 'nan' WHEN f8 = 0 THEN 'zero' ELSE 'other' END AS kind, count(*) AS groups
FROM (SELECT f8 FROM o GROUP BY f8) k GROUP BY 1 ORDER BY 1;

-- the outer WHERE of per-order aggregates: on the group key, on an inner aggregate, over two arms (the plan is the planner's choice at this size, the
-- fixtures of scripts/coldiff.sh check it is taken)
SELECT same($$SELECT category, avg(p) AS a, count(DISTINCT id) AS orders, sum(n) AS items
			  FROM (SELECT o.id, i.category, avg(i.price) AS p, count(*) AS n
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY o.id, i.category) x
			  WHERE category IN ('A', 'C') GROUP BY category$$) AS where_key;
SELECT same($$SELECT category, avg(p) AS a, count(DISTINCT id) AS orders, sum(n) AS items
			  FROM (SELECT o.id, i.category, avg(i.price) AS p, count(*) AS n
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY o.id, i.category) x
			  WHERE p > 5 AND n >= 1 GROUP BY category$$) AS where_aggregate;
SELECT same($$SELECT category, kind, avg(p) AS a, count(DISTINCT id) AS orders
			  FROM (SELECT o.id, i.category, i.kind, avg(i.price) AS p
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8) GROUP BY o.id, i.category, i.kind
					UNION ALL
					SELECT o.id, i.category, NULL::text, avg(i.price)
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8) GROUP BY o.id, i.category) x
			  WHERE kind IS NULL AND p < 8 GROUP BY category, kind$$) AS where_union;

-- grouping sets over per-order aggregates: a rollup over one arm, grouping sets over two (an arm without the kind joins the sets that group by it)
SELECT same($$SELECT category, kind, grouping(category, kind) AS g, avg(p) AS a, count(DISTINCT id) AS orders, sum(n) AS items, count(*) AS cells
			  FROM (SELECT o.id, i.category, i.kind, avg(i.price) AS p, count(*) AS n
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8) GROUP BY o.id, i.category, i.kind) x
			  GROUP BY ROLLUP (category, kind)$$) AS arm_rollup;
SELECT same($$SELECT category, kind, grouping(category, kind) AS g, avg(p) AS a, count(DISTINCT id) AS orders
			  FROM (SELECT o.id, i.category, i.kind, avg(i.price) AS p
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8) GROUP BY o.id, i.category, i.kind
					UNION ALL
					SELECT o.id, i.category, NULL::text, avg(i.price)
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8) GROUP BY o.id, i.category) x
			  GROUP BY GROUPING SETS ((category, kind), (kind), ())$$) AS arms_sets;

-- arms that group by more than the outer keys: the other group expressions are keys of the cells (a row has several cells in a group)
EXPLAIN (COSTS OFF) WITH base AS (SELECT o.id, i.category, i.kind, i.qty, i.price FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8))
SELECT count(DISTINCT id) AS orders, sum(n) AS items, count(*) AS cells
FROM (SELECT id, category, kind, count(*) AS n FROM base GROUP BY id, category, kind UNION ALL SELECT id, category, NULL::text, count(*) FROM base GROUP BY id, category) x;
SELECT same($$WITH base AS (SELECT o.id, i.category, i.kind, i.qty, i.price FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8))
			  SELECT count(DISTINCT id) AS orders, sum(n) AS items, count(*) AS cells, avg(p) AS a
			  FROM (SELECT id, category, kind, count(*) AS n, avg(price) AS p FROM base GROUP BY id, category, kind
					UNION ALL SELECT id, category, NULL::text, count(*), avg(price) FROM base GROUP BY id, category) x$$) AS cells_global;
SELECT same($$WITH base AS (SELECT o.id, i.category, i.kind, i.qty, i.price FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8))
			  SELECT category, count(DISTINCT id) AS orders, sum(n) AS items, count(*) AS cells
			  FROM (SELECT id, category, kind, count(*) AS n FROM base GROUP BY id, category, kind HAVING count(*) > 1
					UNION ALL SELECT id, category, NULL::text, count(*) FROM base GROUP BY id, category) x
			  GROUP BY category$$) AS cells_keyed;
SELECT same($$WITH base AS (SELECT o.id, i.category, i.kind, i.qty, i.price FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8))
			  SELECT category, GROUPING(category) AS g, count(DISTINCT id) AS orders, count(*) AS cells
			  FROM (SELECT id, category, kind, count(*) AS n FROM base GROUP BY id, category, kind
					UNION ALL SELECT id, category, NULL::text, count(*) FROM base GROUP BY id, category) x
			  GROUP BY ROLLUP (category)$$) AS cells_rollup;
SELECT same($$WITH base AS (SELECT o.id, i.category, i.kind, i.qty, i.price FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8))
			  SELECT count(DISTINCT id) AS orders, count(*) AS cells
			  FROM (SELECT id, category, kind, sum(qty) AS n FROM base GROUP BY id, category, kind HAVING sum(qty) > 100
					UNION ALL SELECT id, category, NULL::text, sum(qty) FROM base GROUP BY id, category HAVING sum(qty) > 100) x$$) AS cells_none;

-- aggregates of the columns of a lookup join: count, DISTINCT, min, max, sum and avg of numeric and float8, NULL columns of the LEFT JOIN
SELECT same($$SELECT i.category, count(v.label) AS c, count(DISTINCT v.label) AS d, min(v.label) AS lo, max(v.label) AS hi, sum(v.weight) AS sw, avg(v.weight) AS aw,
					 sum(v.factor) AS sf, avg(v.factor) AS af, sum(v.weight * i.qty) AS wq, count(*) AS n
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN vals v ON v.category = i.category GROUP BY 1$$) AS mapped_left;
SELECT same($$SELECT v.label, max(v.weight) AS hw, sum(v.weight) AS sw, count(DISTINCT v.factor) AS df
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  JOIN vals v ON v.category = i.category GROUP BY ROLLUP (v.label)$$) AS mapped_inner_rollup;
SELECT same($$SELECT count(v.label) AS c, min(v.label) AS lo, sum(v.weight) AS sw, avg(v.factor) AS af
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN vals v ON v.category = i.category WHERE i.category = 'E'$$) AS mapped_no_value;

-- the same from workers: the tables of the grouping sets and of the float keys are merged by value
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
EXPLAIN (COSTS OFF) SELECT kind, store, grouping(kind, store) AS g, grouping(store) AS gs, count(*) AS n, sum(store) AS s FROM o GROUP BY ROLLUP (kind, store);
EXPLAIN (COSTS OFF) SELECT f8, count(*) AS n, sum(id) AS s FROM o GROUP BY f8;
SELECT same($$SELECT kind, store, grouping(kind, store) AS g, grouping(store) AS gs, count(*) AS n, sum(store) AS s FROM o GROUP BY ROLLUP (kind, store)$$) AS rollup_nulls_workers;
SELECT same($$SELECT kind, store % 2 AS odd, grouping(store % 2, kind) AS g, count(*) AS n, count(DISTINCT id) AS ids
			  FROM o GROUP BY CUBE (kind, store % 2) HAVING grouping(kind) = 0 OR count(*) > 300$$) AS cube_having_workers;
SELECT same($$SELECT kind, grouping(kind) AS g, count(*) AS n, sum(store) AS s, min(id) AS m FROM o WHERE store < 0 GROUP BY GROUPING SETS ((kind), ())$$) AS empty_set_workers;
SELECT same($$SELECT i.category, i.kind, grouping(i.category, i.kind) AS g, count(*) AS n, avg(i.price) AS a
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
			  GROUP BY GROUPING SETS ((i.category, i.kind), (i.category), ())$$) AS unnest_sets_workers;
SELECT same($$SELECT f8, count(*) AS n, sum(id) AS s FROM o GROUP BY f8$$) AS float8_key_workers;
SELECT same($$SELECT f8, kind, count(*) AS n FROM o GROUP BY ROLLUP (f8, kind)$$) AS float_key_sets_workers;
-- count(DISTINCT) of an integer column: the workers send the chunks of 4096 bits of their bitmaps that have 32 bits or more and the leader
-- ORs them in, the other bits go as pairs, and so do the sets that left the bitmaps (a sparse range, a tiny work_mem); the rows are the
-- ones of the normal plan either way
CREATE TABLE dd AS SELECT i AS id, i % 3 AS grp, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919) % 3000 - 1000 END AS dense,
	   CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::bigint * 100003) % 2000000011 - 1000000000)::int END AS wide, (i * 31) % 20 AS few
FROM generate_series(1, 6000) i;
CREATE INDEX dd_col ON dd USING sift_colstub (id, grp, dense, wide, few);
VACUUM ANALYZE dd;
CREATE FUNCTION chunks_merged(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Workers Launched: [1-9]' THEN n := n + 1; END IF;
		IF l ~ '[1-9][0-9]* distinct chunks merged' THEN n := n + 100; END IF;
	END LOOP;
	RETURN n;
END $$;
SET parallel_leader_participation = off;
SELECT chunks_merged($$SELECT grp, count(DISTINCT dense) AS dd FROM dd GROUP BY grp$$) AS bitmaps_merged_by_or;
SELECT chunks_merged($$SELECT grp, count(DISTINCT few) AS df FROM dd GROUP BY grp$$) AS sparse_chunks_as_pairs;
SELECT same($$SELECT grp, count(DISTINCT dense) AS dd, count(DISTINCT wide) AS dw, count(DISTINCT few) AS df, count(DISTINCT id) AS di FROM dd GROUP BY grp$$) AS distinct_bitmaps_workers;
SELECT same($$SELECT count(DISTINCT dense) AS dd, count(DISTINCT wide) AS dw, count(DISTINCT few) AS df FROM dd$$) AS distinct_global_workers;
SELECT same($$SELECT grp, count(DISTINCT dense * 100003) AS de FROM dd GROUP BY grp$$) AS distinct_expr_workers;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same($$SELECT grp, count(DISTINCT dense) AS dd, count(DISTINCT wide) AS dw FROM dd GROUP BY grp$$) AS distinct_spilled_workers;
RESET hash_mem_multiplier;
RESET work_mem;
RESET parallel_leader_participation;
RESET max_parallel_workers_per_gather;
-- arms of one UNION ALL that scan the same rows for the same aggregates share one pass; the rows are the ones of the arms alone
CREATE FUNCTION arms_shared(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Arms In One Pass' THEN n := n + 1; END IF;
	END LOOP;
	RETURN n;
END $$;
SELECT arms_shared($$SELECT store AS k, count(*) AS n, sum(id) AS s FROM o GROUP BY store UNION ALL SELECT id % 11, count(*), sum(id) FROM o GROUP BY id % 11$$) AS shared_other_leaves;
SELECT arms_shared($$SELECT id % 5 AS k, count(*) AS n, sum(id) AS s FROM o GROUP BY id % 5 UNION ALL SELECT id % 11, count(*), sum(id) FROM o GROUP BY id % 11 UNION ALL SELECT id % 3, count(*), sum(id) FROM o GROUP BY id % 3$$) AS shared_three;
SELECT arms_shared($$SELECT id % 5 AS k, count(*) AS n, sum(id) AS s FROM o GROUP BY id % 5 UNION ALL SELECT id % 11, count(*), sum(id) FROM o WHERE id > 5 GROUP BY id % 11$$) AS shared_other_filter;
SELECT same($$SELECT store AS k, count(*) AS n, sum(id) AS s, avg(f8) AS a FROM o GROUP BY store UNION ALL SELECT id % 11, count(*), sum(id), avg(f8) FROM o GROUP BY id % 11
			  UNION ALL SELECT id % 3, count(*), sum(id), avg(f8) FROM o GROUP BY id % 3$$) AS shared_arms;
SELECT same($$SELECT 1 AS a, kind AS k, count(*) AS n, sum(f4) AS s FROM o GROUP BY kind UNION ALL SELECT 2, f8::text, count(*), sum(f4) FROM o GROUP BY f8
			  UNION ALL SELECT 3, NULL, count(*), sum(f4) FROM o$$) AS shared_float_null_keys;
SELECT same($$SELECT 1 AS a, i.category AS k, count(*) AS n, sum(i.price) AS s FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8) GROUP BY i.category
			  UNION ALL SELECT 2, i.kind, count(*), sum(i.price) FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8) GROUP BY i.kind$$) AS shared_unnest_arms;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT same($$SELECT store AS k, count(*) AS n, sum(id) AS s, avg(f8) AS a FROM o GROUP BY store UNION ALL SELECT id % 11, count(*), sum(id), avg(f8) FROM o GROUP BY id % 11
			  UNION ALL SELECT id % 3, count(*), sum(id), avg(f8) FROM o GROUP BY id % 3$$) AS shared_arms_workers;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
