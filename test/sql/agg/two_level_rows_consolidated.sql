-- The two-level aggregate of src/agg/two and the node that groups rows (SiftAggRows), one segment each, in a schema of its own: arms per row and category, outer FILTERs, NULL arm columns,
-- the spine, GROUPING SETS, a hashed subplan as key, parallel arms and the memory bound of the grouping. Every query runs as the scan and as the normal plan. Replaces fourteen tests.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> agg_arm_cells
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
SET sift.test_create_format1 = on;
-- The cells of per-row aggregates that group by more than the outer keys (a parent id, an element's tag) live for one row, so the scan numbers
-- them in the row and not in the group table. Every query runs with and without the pushdown; the answers must be the same: cells that
-- are one per row, several per row (a row with more cells than the row's table starts with, and one that spans chunks), HAVING on the cells,
-- grouping sets, an arm without hidden keys next to one with them, and the distinct ids of a hidden key.
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE items (id int PRIMARY KEY, parent_id int, region text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO items
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'r' || i % 9 END,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3], 'tag', CASE WHEN (i + j) % 7 = 0 THEN NULL ELSE 't' || (i * j) % 5 END,
                                                                                  'n', (i * j) % 10))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 375) i;
-- rows with forty cells in a group, and one that spans three chunks of tuples
INSERT INTO items
SELECT 6000 + i, i, 'wide', jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b'])[1 + j % 2], 'tag', 'w' || j, 'n', j % 10))
                                                         FROM generate_series(1, CASE WHEN i = 1 THEN 9000 ELSE 40 END) j))
FROM generate_series(1, 4) i;
CREATE INDEX items_col ON items USING sift_col (id, parent_id, region, doc) WITH (paths = 'doc.parts[]{k text, tag text, n int4}');
VACUUM (FREEZE, ANALYZE) items;

-- the outer aggregates, the arms' extra group column and its HAVING, and the WHERE of the scan
CREATE FUNCTION report(aggs text, hid text DEFAULT 'parent_id', arm_having text DEFAULT '', tail text DEFAULT '', filt text DEFAULT 'true') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH base AS (SELECT t.id, t.parent_id, t.region, p.k, p.tag, p.n
	                                FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, tag text, n int) WHERE %4$s)
	                 SELECT k, %1$s FROM (SELECT id, %2$s, k, sum(n) AS s, count(*) AS c FROM base GROUP BY id, %2$s, k %3$s
	                                      UNION ALL
	                                      SELECT id, %2$s, NULL::text, sum(n), count(*) FROM base GROUP BY id, %2$s) q
	                 GROUP BY k %5$s$q$, aggs, hid, arm_having, filt, tail)
$$;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0 AND pushed(q);
END $$;

-- a hidden key that is a column of the row: one cell per row and outer group
SELECT same(report('sum(s) AS total, sum(c) AS cells, count(*) AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents')) AS row_key;
SELECT same(report('sum(s) AS total, count(DISTINCT region) AS regions', 'region')) AS row_text_key;
-- count(id) of the row ids counts the cells' rows as count(*) does, alone, with a filter, and with a key that splits the cells
SELECT same(report('count(id) AS counted, count(*) AS n, sum(s) AS total')) AS count_row_id;
SELECT same(report('count(id) AS counted, count(id) FILTER (WHERE s > 9) AS big, count(DISTINCT id) AS ids', 'tag')) AS count_row_id_filter;
-- a hidden key that is an element's tag: several cells in a row, NULL tags among them
SELECT same(report('sum(s) AS total, sum(c) AS cells, count(*) AS n, count(DISTINCT id) AS ids, count(DISTINCT tag) AS tags', 'tag')) AS element_key;
-- both, and the arms' HAVING and WHERE
SELECT same(report('sum(s) AS total, count(*) AS n, count(DISTINCT parent_id) AS parents, count(DISTINCT tag) AS tags', 'parent_id, tag')) AS two_keys;
SELECT same(report('sum(s) AS total, count(*) AS n, count(DISTINCT tag) AS tags', 'tag', 'HAVING sum(n) > 2')) AS arm_having;
SELECT same(report('sum(s) AS total, count(*) AS n, count(DISTINCT parent_id) AS parents', 'parent_id', '', '', 'n > 3 AND t.parent_id < 400')) AS where_of_arms;
-- the outer HAVING, and an outer filter on the cells
SELECT same(report('count(DISTINCT parent_id) AS parents, count(*) FILTER (WHERE s > 9) AS big', 'parent_id', '', 'HAVING count(DISTINCT parent_id) > 100')) AS having_outer;
SELECT same(report('count(DISTINCT parent_id) AS parents, count(*) FILTER (WHERE s > 9) AS big', 'parent_id')) AS cell_filter;
-- grouping sets over the outer key
SELECT same(replace(report('sum(s) AS total, count(*) AS n, count(DISTINCT parent_id) AS parents, count(DISTINCT tag) AS tags', 'parent_id, tag'), 'GROUP BY k', 'GROUP BY ROLLUP (k)')) AS rollup;
-- an arm that has no hidden key next to one that has: its cells are the outer groups
SELECT same($q$WITH base AS (SELECT t.id, t.parent_id, p.k, p.n FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int))
                SELECT k, sum(s) AS total, count(*) AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
                FROM (SELECT id, parent_id, k, sum(n) AS s FROM base GROUP BY id, parent_id, k
                      UNION ALL
                      SELECT id, NULL::int, k, sum(n) FROM base GROUP BY id, k) q GROUP BY k$q$) AS mixed_arms;
-- more distinct ids than hash_mem holds
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same(report('sum(s) AS total, count(DISTINCT parent_id) AS parents, count(DISTINCT id) AS ids, count(DISTINCT tag) AS tags', 'parent_id, tag')) AS small_memory;
RESET work_mem;
RESET hash_mem_multiplier;
-- the plan shows the cells' keys
CREATE FUNCTION cell_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Cell Keys' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM cell_lines(report('sum(s) AS total, count(DISTINCT parent_id) AS parents'));

DROP FUNCTION cell_lines(text), same(text), pushed(text), report(text, text, text, text, text);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_arm_cells

-- >>> agg_arm_distinct
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
SET sift.test_create_format1 = on;
-- count(DISTINCT x) of a column the per-row aggregates group by (a parent id, a region), under an outer aggregate: the scan keeps the
-- distinct ids of the column per outer group (a bitmap over the key's ids, a hash set or a sort as count(DISTINCT) of a column does),
-- not the per-row groups that PostgreSQL would sort. Every query runs with and without the pushdown; the answers must be the same.
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
-- the scan is asked for: its price against the normal plan's is a tie at times (a gap of a few percent), the test is of its answers and of the plans it refuses
SET sift.agg_force = on;

CREATE TABLE items (id int PRIMARY KEY, parent_id int, region text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO items
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'r' || i % 9 END,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3], 'n', (i * j) % 10))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 312) i;
CREATE INDEX items_col ON items USING sift_col (id, parent_id, region, doc) WITH (paths = 'doc.parts[]{k text, n int4}');
VACUUM (FREEZE, ANALYZE) items;

-- the outer aggregates given; the arms are per item and key, and per item
CREATE FUNCTION report(aggs text, tail text DEFAULT '', filt text DEFAULT 'true') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH base AS (SELECT t.id, t.parent_id, t.region, p.k, p.n
	                                FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int) WHERE %s)
	                 SELECT k, %s FROM (SELECT id, parent_id, region, k, sum(n) AS s, count(*) AS c FROM base GROUP BY id, parent_id, region, k
	                                    UNION ALL
	                                    SELECT id, parent_id, region, NULL::text, sum(n), count(*) FROM base GROUP BY id, parent_id, region) q
	                 GROUP BY k %s$q$, filt, aggs, tail)
$$;

-- the plan has the scan and neither the grouping of PostgreSQL nor the node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0 AND pushed(q);
END $$;

-- an integer column and a text column, next to the row count and the sums; NULL parents and regions are no value
SELECT same(report('count(DISTINCT parent_id) AS parents')) AS parents;
SELECT same(report('count(DISTINCT region) AS regions')) AS regions;
SELECT same(report('count(DISTINCT parent_id) AS parents, count(DISTINCT region) AS regions, count(DISTINCT id) AS ids, count(*) AS cells, sum(s) AS total')) AS several;
-- the FILTER of an inner aggregate, and the same column with several FILTERs
SELECT same(report('count(DISTINCT parent_id) FILTER (WHERE c > 1) AS many, count(DISTINCT parent_id) FILTER (WHERE s > 9) AS big, count(DISTINCT parent_id) AS parents')) AS filters;
-- a HAVING on the outer query and a WHERE of the arms
SELECT same(report('count(DISTINCT parent_id) AS parents', 'HAVING count(DISTINCT region) > 3')) AS having;
SELECT same(report('count(DISTINCT parent_id) AS parents, count(DISTINCT region) AS regions', '', 'n > 3 AND t.parent_id < 400')) AS where_of_arms;
-- the column is an outer key too, and a grouping set
SELECT same(replace(report('count(DISTINCT parent_id) AS parents, count(DISTINCT region) AS regions'), 'GROUP BY k', 'GROUP BY ROLLUP (k)')) AS rollup;
-- the column of the distinct count is an outer key, next to another column and the ids (many groups: the sets follow the groups as they grow)
SELECT same(replace(report('count(DISTINCT parent_id) AS parents, count(DISTINCT region) AS regions, count(DISTINCT id) AS ids, count(*) AS cells'),
                    'GROUP BY k', 'GROUP BY k, parent_id')) AS outer_key;
-- not a column the arms group by: the distinct values of the inner sums, which the outer groups count like any other value
SELECT same(report('count(DISTINCT s) AS sums')) AS not_a_key;

-- more ids than hash_mem holds: the set moves to a hash and a sort, and the answer is the same
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same(report('count(DISTINCT parent_id) AS parents, count(DISTINCT id) AS ids, count(DISTINCT region) AS regions')) AS small_memory;
RESET work_mem;
RESET hash_mem_multiplier;

-- the plan shows the aggregates
CREATE FUNCTION agg_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Pushed Aggregates' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM agg_lines(report('count(DISTINCT parent_id) AS parents, count(DISTINCT id) AS ids'));

DROP FUNCTION agg_lines(text), same(text), pushed(text), report(text, text, text);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_arm_distinct

-- >>> agg_cell_conditions
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
SET sift.test_create_format1 = on;
-- The outer aggregates of a two-level scan with a FILTER on the value of a cell (an inner average, sum or count) read that value once for the cell and compare it with the
-- constant inline: NaN above every number and equal to itself, infinities, NULL cells (an average of no value), integer and float comparisons. The rows are the normal plan's.
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, cat int, who int, x float8, k int) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, i % 5, i % 211,
       CASE WHEN i % 17 = 0 THEN NULL WHEN i % 101 = 0 THEN 'NaN' WHEN i % 103 = 0 THEN 'Infinity' WHEN i % 107 = 0 THEN '-Infinity' ELSE (hashint4(i) % 2000) / 100.0 END,
       i % 9
FROM generate_series(1, 625) i;
CREATE INDEX ev_col ON ev USING sift_col (cat, who, x, k);
VACUUM (FREEZE, ANALYZE) ev;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_normal_plan bigint) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.agg_rows_and_differences(q) $$;

SELECT * FROM same($q$SELECT cat, count(*) AS cells, count(*) FILTER (WHERE a >= 8) AS hi, count(*) FILTER (WHERE a < 6) AS lo, count(*) FILTER (WHERE a > 6 AND a < 8) AS mid,
  count(*) FILTER (WHERE a = 'NaN') AS nan, count(*) FILTER (WHERE a <> 'NaN') AS not_nan, count(*) FILTER (WHERE a <= 'Infinity') AS le_inf, count(a) AS with_a
  FROM (SELECT cat, who, avg(x) AS a FROM ev GROUP BY 1, 2) c GROUP BY 1$q$);
SELECT * FROM same($q$SELECT cat, count(*) AS cells, sum(n) FILTER (WHERE n > 100) AS big, count(*) FILTER (WHERE n = 190) AS exact, count(*) FILTER (WHERE s >= 3000 AND n < 190) AS mixed, max(s) AS m
  FROM (SELECT cat, who, count(*) AS n, sum(k) AS s FROM ev GROUP BY 1, 2) c GROUP BY 1$q$);
SELECT * FROM same($q$SELECT cat, avg(a) FILTER (WHERE a > 5) AS m1, round((sum(a) FILTER (WHERE a < 5 AND a > -5))::numeric, 6) AS m2, count(a) FILTER (WHERE a >= 'NaN') AS m3
  FROM (SELECT cat, who, avg(x) FILTER (WHERE k > 2) AS a FROM ev GROUP BY 1, 2) c GROUP BY 1$q$);
EXPLAIN (COSTS OFF) SELECT cat, count(*) FILTER (WHERE a >= 8) FROM (SELECT cat, who, avg(x) AS a FROM ev GROUP BY 1, 2) c GROUP BY 1;
SET sift.agg_force = on;
SELECT * FROM same($q$SELECT cat, count(*) FILTER (WHERE a >= 8) AS hi, count(*) FILTER (WHERE a = 'NaN') AS nan, sum(n) AS total
  FROM (SELECT cat, who, avg(x) AS a, count(*) AS n FROM ev GROUP BY 1, 2) c GROUP BY 1$q$);
RESET sift.agg_force;

DROP FUNCTION same(text);
DROP TABLE ev;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_cell_conditions

-- >>> agg_two_level_arm_where
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
SET sift.test_create_format1 = on;
-- The arms of a two-level aggregate may differ in their WHERE: a condition that every arm has is the scan's, and one that only some arms have is a
-- filter of those arms for their tuples (a third arm without the NOT NULL test of a lookup counts the rows the others drop). A constant in the GROUP
-- BY of the arms splits no group. Every query runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as the normal plan; the
-- answers must be the same.
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
-- the scan is asked for: its price against the normal plan moves with the estimate of the join with the ranges of the list (agg_range_join_rows), the test is of its answers
SET sift.agg_force = on;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, 'k' || i % 5, date '2025-01-01' + i % 400,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'dir_a', (array['p', 'n'])[1 + (i + j) % 2],
                                                                                 'score', (i * j) % 10)) FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 1500) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, dir_a text, score float8}');
CREATE TABLE lookup (kind text, cat text, derived_cat text) WITH (autovacuum_enabled = false);
INSERT INTO lookup SELECT 'k' || k, 'cat_' || c, 'm' || (k + c) % 3 FROM generate_series(0, 4) k, generate_series(0, 5) c WHERE (k + c) % 5 <> 0;
CREATE UNIQUE INDEX lookup_key ON lookup (kind, cat);
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) lookup;

-- three arms over the rows with their spans and the lookup (LIMIT 1); w1, w2, w3 are the WHERE of each, bucket is a constant the arms group by
CREATE FUNCTION report(w1 text DEFAULT 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', w2 text DEFAULT 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', w3 text DEFAULT 'true') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH scoped AS NOT MATERIALIZED (
	    SELECT e.id, e.parent_id, e.doc, e.kind, p.span, NULL::date AS bucket
	    FROM events e JOIN (VALUES ('span_a', daterange('2025-07-01', '2026-01-31', '[]')), ('span_b', daterange('2025-01-01', '2025-06-30', '[]'))) p(span, range)
	         ON e.ts <@ p.range),
	  flag_a AS (
	    SELECT s.id, s.parent_id, s.span, s.bucket, CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END AS category, avg(o.score) AS score, count(s.id) AS n
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE %s GROUP BY s.id, s.parent_id, s.span, s.bucket, CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END
	    UNION ALL
	    SELECT s.id, s.parent_id, s.span, s.bucket, NULL::text, avg(o.score), count(s.id)
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE %s GROUP BY s.id, s.parent_id, s.span, s.bucket
	    UNION ALL
	    SELECT s.id, s.parent_id, s.span, s.bucket, NULL::text, avg(o.score), count(s.id)
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE %s GROUP BY s.id, s.parent_id, s.span, s.bucket)
	  SELECT coalesce(category, 'all_cats') AS category, span, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n,
	         count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	  FROM flag_a GROUP BY category, span ORDER BY 1, 2$q$, w1, w2, w3)
$$;

-- the same arms under a spine of buckets, one of which no row has: it is a row of NULL columns
CREATE FUNCTION spined(sets text DEFAULT NULL) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH scoped AS NOT MATERIALIZED (SELECT e.id, e.kind, e.doc, e.ts FROM events e),
	  flag_a AS (
	    SELECT s.id, CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END AS category, avg(o.score) AS score, count(s.id) AS n
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END IS NOT NULL GROUP BY s.id, CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END
	    UNION ALL
	    SELECT s.id, NULL::text, avg(o.score), count(s.id)
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    GROUP BY s.id)
	  SELECT b.label, m.category, count(*) AS cells, round(avg(m.score)::numeric, 6) AS value, sum(m.n)::int AS n, count(DISTINCT m.id) AS ids
	  FROM (VALUES ('m0'), ('m1'), ('cat_2'), ('none')) b(label) LEFT JOIN flag_a m ON m.category = b.label
	  GROUP BY %s ORDER BY 1, 2$q$, coalesce(sets, 'b.label, m.category'))
$$;

-- the plan is the two-level scan: the joins in the node, and neither the grouping of PostgreSQL nor the node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

-- the rows of the two-level scan and of the normal plan are the same; empty: the answer has no row; scan: the plan is the scan's
CREATE FUNCTION same(q text, empty bool DEFAULT false, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND (n > 0) <> empty AND (NOT scan OR pushed(q));
END $$;

-- the third arm has no WHERE: its rows count where the lookup has no row
SELECT same(report()) AS third_arm_unfiltered;
-- the first arm is the one without WHERE
SELECT same(report('true', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL')) AS first_arm_unfiltered;
-- a NOT NULL test of the lookup itself makes its outer join an inner one in the arms that have it: the scan keeps the outer join (a row the lookup has no
-- row for is a tuple of NULLs, which the third arm counts) and the other arms test the NOT NULL
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS join_types_differ;
SELECT same(report('true', 'rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS inner_in_the_last_two;
SELECT same(report('rm.derived_cat IS NOT NULL', 'true', 'rm.derived_cat IS NOT NULL')) AS inner_in_the_ends;
SELECT same(report('rm.derived_cat IS NOT NULL AND o.score > 2', 'true', 'true')) AS inner_in_one_with_a_condition;
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS inner_in_all;
-- the number of tuples the lookup has no row for: the third arm counts them, the others do not
SELECT same($q$SELECT count(*) FILTER (WHERE rm.derived_cat IS NULL) AS unmatched, count(*) AS tuples
	FROM events s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true$q$) AS unmatched_exist;
-- every arm has the same WHERE: it is the scan's
SELECT same(report('CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL')) AS all_alike;
SELECT same(report('true', 'true', 'true')) AS none;
-- each arm has a condition of its own besides the common one
SELECT same(report('CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL AND o.score > 2', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'o.dir_a = ''p'' AND CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL')) AS own_conditions;
SELECT same(report('o.score > 2', 'o.score > 2 AND o.dir_a = ''n''', 'o.score > 2')) AS common_and_own;
-- a condition no tuple passes in one arm: that arm has no group
SELECT same(report('o.score > 100', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'true')) AS arm_without_rows;
SELECT same(report('CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'o.score > 100', 'o.score > 100'), scan => false) AS arms_without_rows;

-- a spine of buckets over arms with different WHERE
SELECT same(spined()) AS spine;
-- and by grouping sets: the sets that lack a key null it, the spine rows no cell matched are a group in each
SELECT same(spined('GROUPING SETS ((b.label, m.category), (b.label), (m.category), ())')) AS spine_sets;

-- rows that were updated and deleted since the index was built are read from the heap
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0 AND doc IS NOT NULL;
UPDATE events SET ts = ts + 30, kind = 'k' || (id + 1) % 5 WHERE id % 19 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT same(report()) AS dirty_rows;
SELECT same(report('CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL AND o.score > 2', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'o.dir_a = ''p''')) AS dirty_own_conditions;
UPDATE lookup SET derived_cat = 'm9' WHERE cat = 'cat_1';
SELECT same(report()) AS dirty_lookup;
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS dirty_join_types_differ;
UPDATE lookup SET derived_cat = NULL WHERE cat = 'cat_3';
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS null_lookup_values;
DELETE FROM lookup WHERE kind = 'k2';
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS missing_lookup_rows;

-- a table joined by an outer join in one arm and an inner one in another is not a lateral LIMIT 1 child: they are not arms of one scan
SELECT same($q$WITH flag_a AS (
	    SELECT e.id, l.derived_cat AS category, count(*) AS n FROM events e LEFT JOIN lookup l ON l.kind = e.kind WHERE l.derived_cat IS NOT NULL GROUP BY e.id, l.derived_cat
	    UNION ALL
	    SELECT e.id, NULL::text, count(*) FROM events e LEFT JOIN lookup l ON l.kind = e.kind GROUP BY e.id)
	  SELECT category, sum(n) AS n, count(DISTINCT id) AS ids FROM flag_a GROUP BY category$q$, scan => false) AS joined_table;

-- an arm that does not join the lookup is not an arm of the same scan, and the refusal says which
CREATE FUNCTION unlike() RETURNS text LANGUAGE sql AS $$
	SELECT $q$WITH flag_a AS (
	    SELECT e.id, rm.derived_cat AS category, count(*) AS n
	    FROM events e LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = e.kind LIMIT 1) rm ON true GROUP BY e.id, rm.derived_cat
	    UNION ALL
	    SELECT e.id, NULL::text, count(*) FROM events e GROUP BY e.id)
	  SELECT category, sum(n) AS n, count(DISTINCT id) AS ids FROM flag_a GROUP BY category$q$
$$;
SELECT same(unlike(), scan => false) AS unlike_arms;
SET sift.explain_refusals = on;
SET client_min_messages = notice;
DO $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || unlike() LOOP
		NULL;
	END LOOP;
END $$;
-- by price: at the default page costs the normal plan and the scans of the arms cost less than the two-level scan did when each arm tuple was priced 330 operations (a measured 70 ns),
-- and the planner kept a plan that runs ten times slower; the scan is chosen with no force
RESET sift.agg_force;
RESET seq_page_cost;
RESET random_page_cost;
SELECT pushed(report()) AS chosen_by_price;
DROP TABLE events, lookup;
DROP FUNCTION pushed(text), report(text,text,text), same(text,boolean,boolean), spined(text), unlike();
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_arm_where

-- >>> agg_two_level_inner_child
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
SET sift.test_create_format1 = on;
-- The arms of a two-level aggregate that read a lateral LIMIT 1 lookup only through its columns (no CASE around them) have the lookup's NOT NULL test
-- in some arms and not in a third: PostgreSQL turns its outer join into an inner one in the arms that test, and leaves the condition on the other
-- table (a pulled up sub-select's WHERE) at the join in those arms and in the WHERE of the third. The arms are still one scan: its outer join, the
-- NOT NULL test as the filter of the arms that have it. The answers are the normal plan's.
-- the tuples through the arms are in the price of the scan: on these few rows the normal plan is the cheaper one, and the test asks about the scan
SET sift.agg_force = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, 'k' || i % 5, date '2025-01-01' + i % 400,
       jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'sub', 'sub_' || (i * j) % 3,
                                                                       'score', CASE WHEN (i + j) % 7 = 0 THEN NULL ELSE (i * j) % 10 END)) FROM generate_series(1, 1 + i % 4) j))
FROM generate_series(1, 200) i;
CREATE INDEX events_col ON events USING sift_col (id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, sub text, score float8}');
CREATE TABLE lookup (kind text, cat text, sub text, derived_cat text, derived_sub text) WITH (autovacuum_enabled = false);
INSERT INTO lookup SELECT 'k' || k, 'cat_' || c, 'sub_' || s, 'm' || (k + c) % 3, 'ms' || (c + s) % 2
FROM generate_series(0, 4) k, generate_series(0, 5) c, generate_series(0, 2) s WHERE (k + c + s) % 4 <> 0;
CREATE UNIQUE INDEX lookup_key ON lookup (kind, cat, sub);
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) lookup;

-- three arms over the rows of the sub-select that keeps the parts with a score and the lookup; w1, w2, w3 are the WHERE of each
CREATE FUNCTION arms(w1 text DEFAULT 'mo.category IS NOT NULL', w2 text DEFAULT 'mo.category IS NOT NULL', w3 text DEFAULT 'true') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH flag_a AS (
	    SELECT s.id, mo.category, mo.sub, avg(mo.score) AS score, count(s.id) AS n
	    FROM events s INNER JOIN LATERAL (
	      SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	      FROM LATERAL (SELECT cat, sub, score FROM jsonb_to_recordset(s.doc['parts']) p(cat text, sub text, score float8) WHERE p.score IS NOT NULL) o
	      LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	    WHERE %s GROUP BY s.id, mo.category, mo.sub
	    UNION ALL
	    SELECT s.id, mo.category, NULL::text, avg(mo.score), count(s.id)
	    FROM events s INNER JOIN LATERAL (
	      SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	      FROM LATERAL (SELECT cat, sub, score FROM jsonb_to_recordset(s.doc['parts']) p(cat text, sub text, score float8) WHERE p.score IS NOT NULL) o
	      LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	    WHERE %s GROUP BY s.id, mo.category
	    UNION ALL
	    SELECT s.id, NULL::text, NULL::text, avg(mo.score), count(s.id)
	    FROM events s INNER JOIN LATERAL (
	      SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	      FROM LATERAL (SELECT cat, sub, score FROM jsonb_to_recordset(s.doc['parts']) p(cat text, sub text, score float8) WHERE p.score IS NOT NULL) o
	      LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	    WHERE %s GROUP BY s.id)
	  SELECT coalesce(category, 'all_cats') AS category, sub, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(DISTINCT id) AS ids
	  FROM flag_a GROUP BY category, sub ORDER BY 1, 2$q$, w1, w2, w3)
$$;

-- the plan is the two-level scan: the joins in the node, and neither the grouping of PostgreSQL nor the node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

-- the rows of the scan and of the normal plan are the same; the plan is the scan's
CREATE FUNCTION same(q text, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0 AND (NOT scan OR pushed(q));
END $$;

-- the lookup's test in the first two arms, none in the third (the sub-select's WHERE is in the inner join's condition of the first two)
SELECT same(arms()) AS inner_in_the_first_two;
-- a condition on the other table's columns besides the test
SELECT same(arms('mo.category IS NOT NULL AND mo.score > 2', 'mo.category IS NOT NULL AND mo.score > 2', 'mo.score > 2')) AS own_and_common;
SELECT same(arms('mo.category IS NOT NULL AND mo.score > 2', 'mo.category IS NOT NULL', 'true')) AS own_in_one;
-- every arm tests
SELECT same(arms('mo.category IS NOT NULL', 'mo.category IS NOT NULL', 'mo.category IS NOT NULL')) AS inner_in_all;
-- a comparison of the lookup's column is a strict test too
SELECT same(arms('mo.category <> ''m0''', 'mo.category <> ''m0''')) AS strict_comparison;

-- rows that were updated and deleted since the index was built are read from the heap; the lookup has NULL values and missing rows
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT same(arms()) AS dirty_rows;
UPDATE lookup SET derived_cat = NULL WHERE cat = 'cat_3';
DELETE FROM lookup WHERE kind = 'k2';
SELECT same(arms()) AS null_and_missing_lookup;
SELECT same(arms('mo.category IS NOT NULL AND mo.score > 2', 'mo.category IS NOT NULL', 'true')) AS dirty_own_condition;

DROP TABLE events, lookup;
DROP FUNCTION arms(text,text,text), pushed(text), same(text,boolean);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_inner_child

-- >>> agg_two_level_inner_minmax
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
SET sift.test_create_format1 = on;
-- max and min of a float8 column or of a float8 key of a jsonb document can be the inner aggregates of the arms of the two-level scan: a query that groups the rows of a
-- table by their id and takes the largest score of each, then averages the scores of a group, runs as one scan. A cell with no value (NULL, a missing key) gives NULL, which
-- the outer avg and sum skip; a NaN is the largest value and an infinity compares as itself. Every query runs as the two-level scan, as the arms alone
-- (sift.agg_two_level off) and as the normal plan; the answers must be the same.
-- the tuples through the arms are in the price of the scan: on these few rows the normal plan is the cheaper one, and the test asks about the scan
SET sift.agg_force = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE rv (id int PRIMARY KEY, grp int, src int, f float8, doc jsonb, deleted bool) WITH (autovacuum_enabled = false);
INSERT INTO rv
SELECT i, CASE WHEN i % 29 = 0 THEN NULL ELSE i % 70 END, i % 4,
       CASE WHEN i % 41 = 0 THEN NULL WHEN i % 313 = 0 THEN 'NaN' WHEN i % 331 = 0 THEN 'Infinity' WHEN i % 337 = 0 THEN '-Infinity' ELSE ((i * 7919) % 1009) / 11.0 END,
       CASE WHEN i % 13 = 0 THEN NULL WHEN i % 7 = 0 THEN '{}'::jsonb
            ELSE jsonb_build_object('mhead', ((i * 31) % 97) / 7.0, 'service', CASE WHEN i % 5 = 0 THEN NULL ELSE (i % 10)::numeric END) END,
       i % 11 = 0
FROM generate_series(1, 200) i;
CREATE INDEX rv_col ON rv USING sift_col (id, grp, src, f, doc, deleted) WITH (paths = 'doc.* float8');
VACUUM (FREEZE, ANALYZE) rv;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_arms)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	RETURN a = 0 AND b = 0 AND n > 0 AND pushed(q);
END $$;

-- the outer query over per-row inner aggregates: the aggregate, the key and the outer tail
CREATE FUNCTION report(inner_agg text, outer_agg text, key text DEFAULT 'grp', inner_where text DEFAULT 'true', inner_having text DEFAULT '') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$SELECT %s, count(id) AS n, %s AS v FROM (SELECT id, grp, src, %s AS x FROM rv WHERE %s GROUP BY id, grp, src %s) t GROUP BY %s$q$,
	              key, outer_agg, inner_agg, inner_where, inner_having, key)
$$;

-- the cases: kind of inner aggregate x outer aggregate; both round to 6 digits because the sums of the plans differ in the last bit
SELECT same(report(a, o, k)) AS ok, a, o, k
FROM (VALUES ('max(f)', 'round(avg(x)::numeric, 6)'), ('min(f)', 'round(avg(x)::numeric, 6)'),
             ('max((doc->>''mhead'')::float8)', 'round(avg(x)::numeric, 6)'), ('min((doc->>''mhead'')::float8)', 'round(sum(x)::numeric, 6)'),
             ('max((doc->>''service'')::float8)', 'round(avg(x)::numeric, 6)'),
             ('max(f)', 'round(sum(x)::numeric, 6)')) c(a, o),
     (VALUES ('grp'), ('src'), ('grp, src')) k(k)
ORDER BY a, o, k;

-- a WHERE of the arm, a HAVING on the inner aggregate, a NULL group key, a join-free filter on deleted rows
SELECT same(report('max(f)', 'round(avg(x)::numeric, 6)', 'grp', 'NOT deleted')) AS inner_where;
SELECT same(report('max((doc->>''mhead'')::float8)', 'round(avg(x)::numeric, 6)', 'src', 'true', 'HAVING max((doc->>''mhead'')::float8) > 5')) AS inner_having;
SELECT same(report('max((doc->>''mhead'')::float8)', 'round(avg(x)::numeric, 6)', 'src', 'src > 0', 'HAVING min((doc->>''mhead'')::float8) < 8')) AS inner_having_min;

-- two arms, one with a WHERE
SELECT same($q$SELECT grp, count(id) AS n, round(avg(x)::numeric, 6) AS v
	FROM (SELECT id, grp, max(f) AS x FROM rv GROUP BY id, grp
	      UNION ALL
	      SELECT id, grp, max((doc->>'mhead')::float8) FROM rv WHERE src = 1 GROUP BY id, grp) t GROUP BY grp$q$) AS two_arms;

DROP TABLE rv;
DROP FUNCTION report(text, text, text, text, text), same(text), pushed(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_inner_minmax

-- >>> agg_two_level_joins
CREATE SCHEMA s7;
GRANT USAGE ON SCHEMA s7 TO PUBLIC;
SET search_path = s7, public;
SET sift.test_create_format1 = on;
-- The per-row aggregates of an outer aggregate (UNION ALL arms grouped by a row id) read joined tables: a VALUES list of spans, a table
-- joined by a key, and a LIMIT 1 lookup. The joins are the scan's, not a plan of their own level that the node could not run. Every query
-- runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as the normal plan; the answers must be the same.
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
-- the scan is asked for: its price against the normal plan's is a tie at times (a gap of a few percent), the test is of its answers and of the plans it refuses
SET sift.agg_force = on;

CREATE TABLE items (id int PRIMARY KEY, kind text, enabled bool) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT i, 'k' || i % 5, i % 7 <> 3 FROM generate_series(0, 599) i;
CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, 'k' || i % 5, date '2025-01-01' + i % 400,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'dir_a', (array['p', 'n'])[1 + (i + j) % 2],
                                                                                 'score', (i * j) % 10)) FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 375) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, dir_a text, score float8}');
CREATE TABLE lookup (kind text, cat text, derived_cat text) WITH (autovacuum_enabled = false);
INSERT INTO lookup SELECT 'k' || k, 'cat_' || c, 'm' || (k + c) % 3 FROM generate_series(0, 4) k, generate_series(0, 5) c WHERE (k + c) % 5 <> 0;
CREATE TABLE lookup_empty (kind text, cat text, derived_cat text);
CREATE TABLE lookup_alt (kind text, cat text, derived_cat text) WITH (autovacuum_enabled = false);
INSERT INTO lookup_alt SELECT kind, cat, 'x' || derived_cat FROM lookup WHERE cat <> 'cat_2';
VACUUM (FREEZE, ANALYZE) items;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) lookup;
ANALYZE lookup_empty;
ANALYZE lookup_alt;

-- the outer aggregate over arms per row and category, and per row; scoped is the rows with their spans and parent, the lookup is LIMIT 1
CREATE FUNCTION report(lk text DEFAULT 'lookup', spans text DEFAULT $p$(VALUES ('span_a', daterange('2025-07-01', '2026-01-31', '[]')),
                       ('span_b', daterange('2025-01-01', '2025-06-30', '[]'))) p(span, range)$p$,
                       parent_join text DEFAULT 'JOIN items pa ON pa.id = e.parent_id AND pa.enabled', lk2 text DEFAULT NULL) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH scoped AS NOT MATERIALIZED (SELECT e.id, e.parent_id, e.doc, e.kind, p.span FROM events e JOIN %s ON e.ts <@ p.range %s),
	  flag_a AS (
	    SELECT s.id, s.parent_id, s.span, rm.derived_cat AS category, avg(o.score) AS score, count(s.id) AS n
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM %s m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE rm.derived_cat IS NOT NULL GROUP BY s.id, s.parent_id, s.span, rm.derived_cat
	    UNION ALL
	    SELECT s.id, s.parent_id, s.span, NULL::text, avg(o.score), count(s.id)
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM %s m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE rm.derived_cat IS NOT NULL GROUP BY s.id, s.parent_id, s.span)
	  SELECT coalesce(category, 'all_cats') AS category, span, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n,
	         count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	  FROM flag_a GROUP BY category, span ORDER BY 1, 2$q$, spans, parent_join, lk, coalesce(lk2, lk))
$$;

-- the spans alone, no lookup: a row in two overlapping spans is two cells of each arm
CREATE FUNCTION overlapping() RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH scoped AS NOT MATERIALIZED (
	    SELECT e.id, e.parent_id, e.doc, p.span
	    FROM events e JOIN (VALUES ('a', daterange('2025-01-01', '2025-09-30', '[]')), ('b', daterange('2025-06-01', '2026-01-31', '[]'))) p(span, range)
	         ON e.ts <@ p.range)
	  SELECT span, category, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	  FROM (SELECT s.id, s.parent_id, s.span, o.cat AS category, avg(o.score) AS score, count(*) AS n
	        FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8) GROUP BY s.id, s.parent_id, s.span, o.cat
	        UNION ALL
	        SELECT s.id, s.parent_id, s.span, NULL::text, avg(o.score), count(*)
	        FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8) GROUP BY s.id, s.parent_id, s.span) t
	  GROUP BY span, category ORDER BY 1, 2$q$)
$$;

-- the plan reads the joins in the scan and has neither the grouping of PostgreSQL nor the node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

-- the rows of the two-level scan, of the arms' scans and of the normal plan are the same; empty: the answer has no row; scan: the plan is the scan's
-- (the two-level scan has no workers: with them the plan may be the arms' scans, whose rows are the same too)
CREATE FUNCTION same(q text, empty bool DEFAULT false, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_arms)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	RETURN a = 0 AND b = 0 AND (n > 0) <> empty AND (NOT scan OR pushed(q));
END $$;

-- the spans, the table joined by its key and the lookup
SELECT same(report()) AS spans_parent_lookup;
-- without the table joined by its key: the NULL parents are no value of count(DISTINCT parent_id)
SELECT same(report(parent_join => '')) AS spans_lookup;
-- overlapping spans: the row of both counts in each
SELECT same(overlapping()) AS overlapping_spans;
-- a lookup without a row: no arm row, no group
SELECT same(report('lookup_empty'), true) AS empty_lookup;

-- arms with the same shape whose lookups are other tables are not one scan: each arm has its own rows
SELECT same(report(lk2 => 'lookup_alt'), scan => false) AS different_lookups;

-- the plan shows the joins
CREATE FUNCTION join_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '(Pushed Aggregates|Lookup Joins|Name)' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM join_lines(report());

-- rows that were updated and deleted since the index was built are read from the heap
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0 AND doc IS NOT NULL;
UPDATE events SET ts = ts + 30, parent_id = (parent_id + 1) % 600 WHERE id % 19 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT same(report()) AS dirty_rows;
SELECT same(overlapping()) AS dirty_overlapping;
UPDATE lookup SET derived_cat = 'm9' WHERE cat = 'cat_1';
SELECT same(report()) AS dirty_lookup;

-- parallel workers, and a hash that does not fit the memory
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT same(report()) AS parallel;
SELECT same(overlapping(), scan => false) AS parallel_overlapping;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same(report()) AS small_memory;
RESET work_mem;
RESET hash_mem_multiplier;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;

DROP FUNCTION join_lines(text), same(text, bool, bool), pushed(text), overlapping(), report(text, text, text, text);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s7 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_joins

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
