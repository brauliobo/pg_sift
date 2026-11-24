-- Format matrix rows K1, S1 and F5: ORDER BY .. LIMIT keyed by a boolean, float4 or float8 column (NaN, infinities, NULLs, both directions), aggregates, FILTER, HAVING and WHERE built only from
-- constants and stable functions, and a text match with a column qual in a Top-N. Every statement answers the rows the heap answers with sift.enable_agg = off, on a sift_col index and on a sift_table;
-- plan_ok prints t when it ran as SiftAggV6.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET sift.test_create_format1 = off;
SET sift.agg_force = on;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE mh (id int PRIMARY KEY, cat int, b bool, f4 float4, f8 float8, t text, body text) WITH (autovacuum_enabled = false);
INSERT INTO mh
SELECT g, g % 40, CASE WHEN g % 7 = 0 THEN NULL ELSE g % 3 = 0 END,
	   CASE g % 11 WHEN 0 THEN NULL WHEN 1 THEN 'NaN' WHEN 2 THEN 'Infinity' WHEN 3 THEN '-Infinity' WHEN 4 THEN 0 WHEN 5 THEN '-0' ELSE (g % 50) - 25.5 END,
	   CASE g % 13 WHEN 0 THEN NULL WHEN 1 THEN 'NaN' WHEN 2 THEN 'Infinity' WHEN 3 THEN '-Infinity' WHEN 4 THEN 0 WHEN 5 THEN '-0' ELSE (g % 60) - 30.25 END,
	   'k' || g % 9, (ARRAY['gift card', 'gift shop', 'plain', 'card'])[1 + g % 4]
FROM generate_series(1, 3000) g;
CREATE TABLE mc (LIKE mh INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE mt (LIKE mh INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO mc SELECT * FROM mh;
INSERT INTO mt SELECT * FROM mh;
CREATE INDEX mc_col ON mc USING sift_col (id, cat, b, f4, f8, t);
CREATE INDEX mc_fts ON mc USING sift (body);
SELECT sift.table_maintain('mt') IS NOT NULL AS maintained;
ANALYZE mh; ANALYZE mc; ANALYZE mt;

CREATE FUNCTION rows_of(q text, agg boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n''), '''') FROM (%s) r', q) INTO r;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN r;
END $$;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT rows_of(q, true) = rows_of(q, false) $$;

CREATE FUNCTION plan_ok(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP n := n + (line LIKE '%SiftAggV6%')::int; END LOOP;
	RETURN n = 1;
END $$;

CREATE FUNCTION topn_lines(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP n := n + (line LIKE '%Top-N:%')::int; END LOOP;
	RETURN n;
END $$;

-- K1: the sort keys are a boolean, a float4 or a float8, with every NULL placement and direction; the tie-break is the id as in the heap
CREATE TEMP TABLE k_keys (n int, k text);
INSERT INTO k_keys VALUES (1, 'b'), (2, 'b DESC'), (3, 'b NULLS FIRST'), (4, 'b DESC NULLS LAST'), (5, 'f4'), (6, 'f4 DESC'), (7, 'f4 NULLS FIRST'), (8, 'f4 DESC NULLS LAST'),
	(9, 'f8'), (10, 'f8 DESC'), (11, 'f8 NULLS FIRST'), (12, 'f8 DESC NULLS LAST'), (13, 'b, f4 DESC, id'), (14, 'f8 DESC, b, f4'), (15, 'cat, b DESC, f8');
SELECT n, parity(format('SELECT id, b, f4, f8 FROM mc WHERE cat < 30 ORDER BY %s, id LIMIT 25', k)) AS index_rows,
	   parity(format('SELECT id, b, f4, f8 FROM mt WHERE cat < 30 ORDER BY %s, id LIMIT 25', k)) AS table_rows,
	   plan_ok(format('SELECT id, b, f4, f8 FROM mc WHERE cat < 30 ORDER BY %s, id LIMIT 25', k)) AS index_plan,
	   plan_ok(format('SELECT id, b, f4, f8 FROM mt WHERE cat < 30 ORDER BY %s, id LIMIT 25', k)) AS table_plan FROM k_keys ORDER BY n;

-- the page after the first: the extremes of the float order (NaN above the infinities, -0 equal to 0) are rows of the heap's answer too
SELECT parity('SELECT id, f4 FROM mc ORDER BY f4 DESC, id LIMIT 12 OFFSET 40') AS k_offset_index, parity('SELECT id, f8 FROM mt ORDER BY f8, id LIMIT 12 OFFSET 40') AS k_offset_table;
SELECT (SELECT f8 FROM mc ORDER BY f8 DESC NULLS LAST, id LIMIT 1) = 'NaN' AS k_nan_first_desc, (SELECT f8 FROM mc ORDER BY f8, id LIMIT 1) = '-Infinity' AS k_ninf_first_asc,
	   (SELECT f4 FROM mc ORDER BY f4 DESC NULLS LAST, id LIMIT 1 OFFSET 220) IS NOT DISTINCT FROM (SELECT f4 FROM mh ORDER BY f4 DESC NULLS LAST, id LIMIT 1 OFFSET 220) AS k_offset_value;

-- -0 and 0 are one key: the rows tie and the id tells them apart, the value shown is the row's own
SELECT parity('SELECT id, f4, f8 FROM mc WHERE f4 = 0 ORDER BY f4, id LIMIT 30') AS k_zero_index, parity('SELECT id, f4, f8 FROM mt WHERE f8 = 0 ORDER BY f8 DESC, id LIMIT 30') AS k_zero_table;

-- EXPLAIN names the Top-N keys once
SELECT topn_lines('SELECT id, cat FROM mc ORDER BY cat, id LIMIT 25') AS k_topn_lines_index, topn_lines('SELECT id, f8 FROM mc WHERE cat < 30 ORDER BY f8, id LIMIT 25') AS k_topn_lines_float,
	   topn_lines('SELECT id, b FROM mc WHERE cat < 30 ORDER BY b, id LIMIT 25') AS k_topn_lines_rows;

-- a table with no rows, and a filter that keeps none
CREATE TABLE m0 (id int, cat int, b bool, f8 float8, t text) WITH (autovacuum_enabled = false);
CREATE INDEX m0_col ON m0 USING sift_col (id, cat, b, f8, t);
CREATE TABLE m0t (LIKE m0) USING sift_table WITH (autovacuum_enabled = false);
SELECT parity('SELECT id, b FROM m0 ORDER BY b, id LIMIT 5') AS k_empty, parity('SELECT id, f8 FROM mc WHERE cat > 99 ORDER BY f8, id LIMIT 5') AS k_none;

-- S1: an aggregate argument, a FILTER, a HAVING or a WHERE of constants and stable functions only; the table of no rows and a NULL constant answer as the heap does
CREATE TEMP TABLE s_q (n int, q text);
INSERT INTO s_q VALUES
	(1, 'SELECT t, count(*) FROM %s WHERE (now())::text IS NOT NULL GROUP BY 1 ORDER BY 1'),
	(2, 'SELECT t, count((now())::text) FROM %s GROUP BY 1 ORDER BY 1'),
	(3, 'SELECT t, count(*) FILTER (WHERE (now())::text IS NOT NULL) FROM %s GROUP BY 1 ORDER BY 1'),
	(4, 'SELECT t, count(*) FROM %s GROUP BY 1 HAVING bool_and((now())::text IS NOT NULL) ORDER BY 1'),
	(5, 'SELECT t, count((pi())::text) FROM %s GROUP BY 1 ORDER BY 1'),
	(6, 'SELECT t, count(*) FILTER (WHERE (pi())::text IS NOT NULL) FROM %s GROUP BY 1 ORDER BY 1'),
	(7, 'SELECT t, count(*) FROM %s GROUP BY 1 HAVING bool_and((pi())::text IS NOT NULL) ORDER BY 1'),
	(8, 'SELECT t, count(*) FROM %s WHERE (statement_timestamp())::text IS NOT NULL GROUP BY 1 ORDER BY 1'),
	(9, 'SELECT t, count((statement_timestamp())::text) FROM %s GROUP BY 1 ORDER BY 1'),
	(10, 'SELECT t, count(*) FILTER (WHERE (transaction_timestamp())::text IS NOT NULL) FROM %s GROUP BY 1 ORDER BY 1'),
	(11, 'SELECT t, count(*) FROM %s GROUP BY 1 HAVING bool_and((transaction_timestamp())::text IS NOT NULL) ORDER BY 1'),
	(12, 'SELECT count(''x''), sum(1), min(2), max(''y''::text), count(NULL::int), sum(NULL::int) FROM %s'),
	(13, 'SELECT t, count(''x''), sum(1), count(*) FILTER (WHERE true) FROM %s GROUP BY 1 ORDER BY 1'),
	(14, 'SELECT t, count(*) FILTER (WHERE NULL), count(*) FILTER (WHERE false), sum(cat) FILTER (WHERE now() < now() - interval ''1 day'') FROM %s GROUP BY 1 ORDER BY 1'),
	(15, 'SELECT t, count(*) FROM %s WHERE (now())::text IS NULL GROUP BY 1 ORDER BY 1'),
	(16, 'SELECT t, count(*), max(now()) = max(now()) FROM %s WHERE (now())::text IS NOT NULL AND cat < 20 GROUP BY 1 ORDER BY 1'),
	(17, 'SELECT t, count(*) FROM %s WHERE (now() > ''2000-01-01'') = (random() < 2) GROUP BY 1 ORDER BY 1'),
	(18, 'SELECT count(*), count(now()), sum(1.5) FROM %s WHERE NULL::boolean');
SELECT n, parity(format(q, 'mc')) AS index_rows, parity(format(q, 'mt')) AS table_rows, parity(format(q, 'm0')) AS empty_index_rows, parity(format(q, 'm0t')) AS empty_table_rows
	FROM s_q ORDER BY n;
SELECT n, plan_ok(format(q, 'mc')) AS index_plan, plan_ok(format(q, 'm0')) AS empty_plan, plan_ok(format(q, 'mt')) AS table_plan FROM s_q WHERE n NOT IN (17, 18) ORDER BY n;

-- F5: a text match and a column qual in the same Top-N
CREATE TEMP TABLE f_q (n int, q text);
INSERT INTO f_q VALUES
	(1, 'SELECT id, f4 FROM mc WHERE body @@> ''gift'' AND cat < 20 ORDER BY f4 DESC NULLS LAST, id LIMIT 10'),
	(2, 'SELECT id, cat FROM mc WHERE body @@> ''gift'' AND cat < 20 ORDER BY cat DESC, id LIMIT 10'),
	(3, 'SELECT id, b FROM mc WHERE body @@> ''card'' AND cat % 3 = 1 ORDER BY b, id LIMIT 10 OFFSET 5'),
	(4, 'SELECT id, cat FROM mc WHERE body @@> ''gift'' AND cat IS NOT NULL ORDER BY cat, id LIMIT 10'),
	(5, 'SELECT id, f8 FROM mc WHERE body @@> ''absent'' AND cat < 20 ORDER BY f8, id LIMIT 10'),
	(6, 'SELECT id, f8 FROM mc WHERE body @@> ''gift'' AND t = ''k3'' AND cat > 10 ORDER BY f8 DESC NULLS LAST, id LIMIT 10');
SELECT n, parity(q) AS rows, plan_ok(q) AS plan FROM f_q ORDER BY n;

DROP FUNCTION rows_of(text, boolean), parity(text), plan_ok(text), topn_lines(text);
DROP TABLE mh, mc, mt, m0, m0t;
DROP EXTENSION pg_sift;
