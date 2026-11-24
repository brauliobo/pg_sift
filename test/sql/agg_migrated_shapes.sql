-- The aggregate shapes the column-source stub tests (aggstub_*) held to the normal plan: generic and ordered-set aggregates, jsonb leaves, existential tests over arrays of objects, grouping sets, float keys, expressions, lookup joins and parallel scans.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
\set VERBOSITY terse

-- the rows of q as sorted jsonb text, or its error, with the aggregate scan on or off
CREATE FUNCTION mig_run(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), ''[]'')::text FROM (%s) t', q) INTO r;
	RETURN r;
EXCEPTION WHEN others THEN RETURN 'ERR ' || SQLSTATE || ' ' || SQLERRM;
END $$;
-- 'same' when the rows (or the error) of q are those of the normal plan, else both cut to 200 characters
CREATE FUNCTION mig_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := mig_run(q, false); b text := mig_run(q, true);
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;
-- the control: a statement whose rows depend on the setting is told apart
SELECT mig_same($$SELECT current_setting('sift.enable_agg') AS v$$) LIKE 'DIFF%' AS control_detects_a_difference;

-- >>> agg_shapes
SET client_min_messages = warning;
CREATE SCHEMA m1;
GRANT USAGE ON SCHEMA m1 TO PUBLIC;
SET search_path = m1, public;
-- Custom Scan (SiftAgg) over the test column source: the plan is taken for the shapes it proves, and its rows are the ones of the
-- normal plan (sift.enable_agg off) for aggregates, group keys, filters, NULLs and unnested arrays of objects.
SET max_parallel_workers_per_gather = 0;
-- the tables are tiny: the scan's start-up cost would keep the btree plans these tests are not about
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
CREATE TABLE o (id int PRIMARY KEY, store int, placed date, kind text, amount float8, qty int, note text, items jsonb, pad text)
	WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o
SELECT g, g % 7, date '2024-01-01' + g % 400, (ARRAY['web', 'shop', 'phone'])[1 + g % 3], CASE WHEN g % 11 = 0 THEN NULL ELSE (g % 100) * 1.5 END,
	   CASE WHEN g % 13 = 0 THEN NULL ELSE g % 17 END, CASE WHEN g % 5 = 0 THEN NULL ELSE 'n' || g % 9 END,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE jsonb_build_array(jsonb_build_object('category', (ARRAY['A', 'B', 'C', 'D'])[1 + g % 4], 'qty', g % 5, 'price', (g % 9) * 2.5),
															 jsonb_build_object('category', (ARRAY['A', 'B', 'C', 'D'])[1 + (g / 3) % 4], 'qty', g % 3, 'price', (g % 7) * 1.5)) END,
	   repeat('x', 100)
FROM generate_series(1, 6000) g;
CREATE INDEX o_col ON o USING sift_col (id, store, placed, kind, amount, qty, note, items) WITH (paths = 'items[]{category text, qty int4, price float8}');
VACUUM ANALYZE o;
-- the plan of a row level and of an unnesting query
-- ORDER BY in the call: a generic aggregate over the sorted rows
-- a range of a column that follows the heap order is priced by the units its zone leaves, and is cheaper than the btree scan
-- the rows of the two plans
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_rows_equal_and_not_empty(q) $$;
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
CREATE INDEX w_col ON w USING sift_col (id, a, b, c);
VACUUM ANALYZE w;
SELECT same($$SELECT a, count(*) AS n, sum(b) AS s FROM w GROUP BY a$$) AS wide_key;
SELECT same($$SELECT a, c, count(*) AS n FROM w GROUP BY a, c$$) AS wide_key_and_null;
SELECT same($$SELECT b, count(*) AS n FROM w WHERE a IN (1000003, 2000006, 3000009, 1999999000, 5, -2000000000) GROUP BY b$$) AS wide_in;
SELECT same($$SELECT b, count(*) AS n FROM w WHERE a NOT IN (1000003, 2000006, 3000009, 1999999000, 5, -2000000000) GROUP BY b$$) AS wide_not_in;
-- count(DISTINCT): the same rows as the normal plan, and at the smallest work_mem the sets spill to a sort instead of growing
SELECT same($$SELECT b, count(DISTINCT a) AS da, count(DISTINCT c) AS dc, count(DISTINCT id) AS di FROM w GROUP BY b$$) AS distinct_groups;
SELECT same($$SELECT count(DISTINCT a) AS da, count(DISTINCT c) AS dc FROM w$$) AS distinct_global;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same($$SELECT b, count(DISTINCT a) AS da, count(DISTINCT c) AS dc, count(DISTINCT id) AS di FROM w GROUP BY b$$) AS distinct_spilled;
SELECT same($$SELECT count(DISTINCT a) AS da, count(DISTINCT id) AS di FROM w$$) AS distinct_global_spilled;
RESET hash_mem_multiplier;
RESET work_mem;
-- ordered-set aggregates: the plan, and the rows of the normal plan with the histogram and, at the smallest work_mem, with the sort
SELECT same($$SELECT kind, percentile_cont(0.5) WITHIN GROUP (ORDER BY amount) AS c, percentile_cont(0.9) WITHIN GROUP (ORDER BY amount DESC) AS cd,
			  percentile_disc(0.3) WITHIN GROUP (ORDER BY qty) AS dq, percentile_disc(0.7) WITHIN GROUP (ORDER BY placed DESC) AS dp FROM o GROUP BY kind$$) AS percentiles;
SET work_mem = '64kB';
SELECT same($$SELECT i.category, percentile_cont(0.5) WITHIN GROUP (ORDER BY i.price * 1.1 + o.id) AS c, percentile_disc(0.05) WITHIN GROUP (ORDER BY i.price * 1.1 + o.id) AS d
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY 1$$) AS spilled;
RESET work_mem;
-- a CTE that several arms read is planned inline when the arms run on the index; the rows are those of the materialized CTE
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m1 CASCADE;
-- <<< agg_shapes

-- >>> exists
SET client_min_messages = warning;
CREATE SCHEMA m2;
GRANT USAGE ON SCHEMA m2 TO PUBLIC;
SET search_path = m2, public;
CREATE FUNCTION forced(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	RETURN mig_same(q);
END $$;
-- Existential tests over the elements of an array of objects (EXISTS, jsonb_path_exists, @>) on the test column source: the plan takes
-- the shapes it proves and its rows are the ones of the normal plan for NULL, JSON null, empty and odd documents; the near misses
-- keep the normal plan.
SET max_parallel_workers_per_gather = 0;
-- doc: any shape (nullable); dc: arrays of objects only (what jsonb_to_recordset accepts), NOT NULL
CREATE TEMP TABLE pat (k int, doc jsonb, dc jsonb);
INSERT INTO pat VALUES
 (0, NULL, '[]'),
 (1, 'null', '[{"type":"kind_a","role":"type_a","n":1}]'),
 (2, '[]', '[{"type":"kind_a","role":null,"n":2},{"type":"x"}]'),
 (3, '[null]', '[{"type":"","n":3}]'),
 (4, '[1, 2]', '[{"role":"type_a"}]'),
 (5, '[{}]', '[{"type":"kïnd_x","role":"type_a","n":-1},{"type":"kind_a","role":"type_b","n":5}]'),
 (6, '[{"type":"kind_a"}]', '[{"type":"kind_a","n":2},{"type":"kind_a","role":"type_a"}]'),
 (7, '[{"type":"kind_a","role":"type_a"},{"type":"x","role":null}]', '[]'),
 (8, '[{"type":"","role":"type_a","n":3},{"type":"kïnd_x","n":-1}]', '[{"type":"x","role":"type_a","n":4}]'),
 (9, '[{"type":"a"},{"role":"b"}]', '[{"type":null,"role":"b"}]');
-- r, r8: columns of the row an element is compared with (int4 and bigint, NULL in one row of seven)
CREATE TABLE d (id int PRIMARY KEY, g int, r int, r8 bigint, doc jsonb, dc jsonb NOT NULL, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO d SELECT n * 10 + p.k, n % 3, CASE WHEN (n + p.k) % 7 = 0 THEN NULL ELSE (n + p.k) % 7 - 1 END,
	CASE WHEN (n + p.k) % 7 = 0 THEN NULL WHEN (n + p.k) % 11 = 0 THEN 5000000000 ELSE (n + p.k) % 6 END, p.doc, p.dc, repeat('x', 200)
FROM generate_series(1, 150) n, pat p;
CREATE INDEX d_col ON d USING sift_col (id, g, r, r8, doc, dc)
	WITH (paths = 'doc[]{type text, role text, n int4}, dc[]{type text, role text, n int4}');
VACUUM ANALYZE d;
-- 'pushed' when SiftAgg ran (the sequential scan is off so that the cost does not decide), 'plain' when it did not; then whether the
-- rows are the normal plan's
-- jsonb_path_exists and @?: strings, integers, && and ||, the empty string, a non-ASCII string, NULL and odd documents
SELECT forced($$SELECT g, count(*) AS n, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.type == "kind_a" && @.role == "type_a")')) AS f FROM d GROUP BY g$$) AS path_and;
SELECT forced($$SELECT g, count(*) FILTER (WHERE doc @? '$[*] ? (@.type == "" || @.n < 0)') AS f FROM d GROUP BY g$$) AS path_or_empty_string;
SELECT forced($$SELECT g, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (3 == @.n)')) AS f FROM d GROUP BY g$$) AS path_integer;
SELECT forced($$SELECT g, count(*) AS n FROM d WHERE jsonb_path_exists(doc, '$[*] ? (@.type == "kïnd_x")') GROUP BY g$$) AS path_where;
-- @>: every object of the right side is contained by some element, not necessarily the same one
SELECT forced($$SELECT g, count(*) FILTER (WHERE doc @> '[{"type": "kind_a"}]') AS one, count(*) FILTER (WHERE doc @> '[{"role": "type_a"}, {"type": "kïnd_x"}]') AS two,
				count(*) FILTER (WHERE '[{"n": 3, "role": "type_a"}]' <@ doc) AS commuted FROM d GROUP BY g$$) AS contains;
-- EXISTS and NOT EXISTS: never NULL, an absent or empty array has no element
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(type text, role text, n int) WHERE e.type = 'kind_a' AND e.role = 'type_a')) AS yes,
				count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(type text, n int) WHERE e.n > 1 OR e.type IS NULL)) AS no FROM d GROUP BY g$$) AS exists_not_exists;
SELECT forced($$SELECT count(*) FROM d WHERE EXISTS (SELECT 1 FROM jsonb_array_elements(d.dc) e WHERE e->>'role' = 'type_a')$$) AS exists_elements;
-- NOT of a test that is NULL for a NULL document only on a NOT NULL column
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT jsonb_path_exists(dc, '$[*] ? (@.type == "kind_a")')) AS f FROM d GROUP BY g$$) AS not_path_not_null;
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT dc @> '[{"role": "type_a"}]') AS f FROM d GROUP BY g$$) AS not_contains_not_null;
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT jsonb_path_exists(doc, '$[*] ? (@.type == "kind_a")')) AS f FROM d GROUP BY g$$) AS not_path_nullable;
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT doc @> '[{"type": "kind_a"}]') AS f FROM d GROUP BY g$$) AS not_contains_nullable;
-- an element against a column of its own row: every operator on either side, the int4 member against a bigint column, the NULL
-- column and the NULL member are unknown, NOT EXISTS keeps the rows with no comparison that is true
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.r)) AS eq,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n <> d.r)) AS ne,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n < d.r)) AS lt,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE d.r <= e.n)) AS le,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n > d.r)) AS gt,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n >= d.r)) AS ge FROM d GROUP BY g$$) AS exists_outer_column;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.r8)) AS eq,
				count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n < d.r8)) AS lt FROM d GROUP BY g$$) AS exists_outer_column_bigint;
SELECT forced($$SELECT g, count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.r)) AS none_eq,
				count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n IS NULL OR e.n > d.r)) AS none_null_or_gt FROM d GROUP BY g$$) AS not_exists_outer_column;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(type text, n int) WHERE e.type = 'kind_a' AND e.n = d.g)) AS f FROM d GROUP BY g$$) AS exists_outer_column_same_element;
-- PostgreSQL turns the negated conjunction into the OR of the negated comparisons, which are unknown for a NULL as the others are
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(type text, n int) WHERE NOT (e.type = 'kind_a' AND e.n = d.r))) AS f FROM d GROUP BY g$$) AS exists_outer_not_and;
-- near misses keep the normal plan: != (a JSON null member is "not equal" in jsonpath), a float literal, a string against an integer,
-- a JSON null or a float in a containment, an element compared with an expression of the outer row, a column of another type class and a limit that is no constant
SELECT forced($$SELECT g, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.type != "kind_a")')) AS f FROM d GROUP BY g$$) AS path_not_equal;
SELECT forced($$SELECT g, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n > 1.5)')) AS f FROM d GROUP BY g$$) AS path_float;
SELECT forced($$SELECT g, count(*) FILTER (WHERE jsonb_path_exists(doc, '$[*] ? (@.n == "3")')) AS f FROM d GROUP BY g$$) AS path_string_vs_integer;
SELECT forced($$SELECT g, count(*) FILTER (WHERE doc @> '[{"role": null}]') AS f FROM d GROUP BY g$$) AS contains_null;
SELECT forced($$SELECT g, count(*) FILTER (WHERE doc @> '[{"n": 2.5}]') AS f FROM d GROUP BY g$$) AS contains_float;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.g + 1)) AS f FROM d GROUP BY g$$) AS exists_outer_expression;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.g::float8)) AS f FROM d GROUP BY g$$) AS exists_outer_other_class;
SELECT forced($$SELECT g, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(d.dc) AS e(n int) WHERE e.n = d.r LIMIT (SELECT 1))) AS f FROM d GROUP BY g$$) AS exists_outer_limit;
-- the shared database of a plain installcheck goes on with the next test
DROP TABLE pat;
SET client_min_messages = warning;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m2 CASCADE;
-- <<< exists

-- >>> expr
SET client_min_messages = warning;
CREATE SCHEMA m3;
GRANT USAGE ON SCHEMA m3 TO PUBLIC;
SET search_path = m3, public;
-- Expressions without a kernel on the test column source: PostgreSQL evaluates them over the decoded leaves (integer, date, float and
-- dictionary leaves) as aggregate arguments, group keys and filters, with the rows of the normal plan (sift.enable_agg off); a division
-- that the normal plan never reaches does not raise, and a small combo cache that is dropped many times changes nothing.
CREATE TABLE e (id int, g int, b int8, d date, f float8, t text, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO e
SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919) % 100003 - 50000 END, date '2024-01-01' + i % 400,
	   CASE WHEN i % 13 = 0 THEN NULL WHEN i % 29 = 0 THEN 'NaN' ELSE (i % 64) / 4.0 - 3 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || (i % 9) END, NULL
FROM generate_series(1, 6000) i;
CREATE INDEX e_col ON e USING sift_col (id, g, b, d, f, t);
ANALYZE e;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;
-- an integer sum PostgreSQL would evaluate: a generic aggregate
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_equals_normal_nonempty(q) $$;
SELECT same($$SELECT g, sum(f / 3) AS a, max(b % 7) AS b, min(b / 3) AS c, count(id / 2) AS d, max(sqrt(abs(f))) AS e FROM e GROUP BY g$$) AS arguments;
SELECT same($$SELECT b % 7 AS k, date_trunc('month', d) AS m, upper(t) AS u, count(*) AS n, max(id % 100) AS x FROM e GROUP BY 1, 2, 3$$) AS keys;
SELECT same($$SELECT g, count(*) FILTER (WHERE sqrt(abs(f)) > 1) AS a, count(*) FILTER (WHERE b / 3 > 100 AND d > date '2024-06-01') AS b,
			  count(*) FILTER (WHERE id % 5 = ANY (ARRAY[1, 2]::int8[])) AS c, count(*) FILTER (WHERE f::numeric > 1.5) AS e FROM e GROUP BY g$$) AS filters;
-- the divisor is zero for g = 0 and b = 0, in rows the guard excludes
SELECT same($$SELECT g, sum(id / g) FILTER (WHERE g <> 0) AS a, count(*) FILTER (WHERE b <> 0 AND 1000 / b > 2) AS b, max(CASE WHEN g = 0 THEN 0 ELSE 50 / g END) AS c FROM e GROUP BY g$$) AS guarded;
SELECT same($$SELECT count(*) FROM e WHERE g <> 0 AND id / g > 3$$) AS guarded_where;
SET work_mem = '64kB';
SELECT same($$SELECT g, sum(f / 3) AS a, max(b % 7 + id % 13) AS b FROM e GROUP BY g$$) AS combo_cache;
RESET work_mem;
SELECT same($$SELECT g, sum(id / 2) AS a, avg(b % 4) AS b FROM e GROUP BY g$$) AS int_sums;
DROP TABLE e;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m3 CASCADE;
-- <<< expr

-- >>> generic
SET client_min_messages = warning;
CREATE SCHEMA m4;
GRANT USAGE ON SCHEMA m4 TO PUBLIC;
SET search_path = m4, public;
-- Custom Scan (SiftAgg) over the test column source: aggregates without a kernel run PostgreSQL's own transition and final functions on the
-- decoded values (sum of float4, the variance family, bit and boolean aggregates, min/max of text, DISTINCT, string_agg, array_agg,
-- ordered-set aggregates of any type, user aggregates). The rows are the ones of the normal plan (sift.enable_agg off); what keeps the plan is listed.
SET max_parallel_workers_per_gather = 0;
-- the tables are tiny: the scan's start-up cost would keep the btree plans these tests are not about
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
CREATE TABLE o (id int PRIMARY KEY, store int, kind text, price int4, f8 float8, f4 float4, day date, tag varchar, items jsonb, pad text)
	WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o
SELECT g, g % 7, CASE WHEN g % 5 = 0 THEN NULL ELSE (ARRAY['web', 'shop', 'phone'])[1 + g % 3] END, (g % 97) * 137 + 5,
	   (g % 23) * 0.37, (g % 13) * 0.11, date '2024-01-01' + g % 40, CASE WHEN g % 11 = 0 THEN NULL ELSE 't' || g % 6 END,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE jsonb_build_array(
			jsonb_build_object('category', (ARRAY['A', 'b', 'C'])[1 + g % 3], 'qty', g % 5, 'price', (g % 9) * 2.5),
			jsonb_build_object('category', (ARRAY['A', 'b', 'C'])[1 + (g / 3) % 3], 'qty', g % 3, 'price', (g % 7) * 1.5)) END,
	   repeat('x', 100)
FROM generate_series(1, 1500) g;
CREATE INDEX o_col ON o USING sift_col (id, store, kind, price, f8, f4, day, tag, items) WITH (paths = 'items[]{category text, qty int4, price float8}');
VACUUM ANALYZE o;
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_equals_normal(q) $$;
CREATE FUNCTION sq_add(acc float8, x float8) RETURNS float8 LANGUAGE sql IMMUTABLE AS $$ SELECT acc + x * x $$;
CREATE AGGREGATE sum_sq(float8) (sfunc = sq_add, stype = float8, initcond = '0');
CREATE FUNCTION vol_add(acc float8, x float8) RETURNS float8 LANGUAGE sql VOLATILE AS $$ SELECT acc + x + 0 * random() $$;
CREATE AGGREGATE sum_vol(float8) (sfunc = vol_add, stype = float8, initcond = '0');
-- the plans: the aggregates are pushed with their arguments (ORDER BY in the call, hypothetical-set aggregates and floats shown as arrays too); a volatile transition function keeps the plan
-- numbers: sum(float4) adds in float4 in the order of the heap, the variance family accumulates in numeric (exact) or in float8
SELECT same($$SELECT kind, round(sum(f4)::numeric, 3) AS s, round(sum(f4) FILTER (WHERE store > 2)::numeric, 3) AS sf, round(avg(f4)::numeric, 6) AS a FROM o GROUP BY kind$$) AS sum_float4;
SELECT same($$SELECT kind, stddev_samp(price) AS sd, var_pop(price) AS vp, variance(store) AS vs, stddev_pop(store) AS ss, bit_and(store) AS ba, bit_or(id) AS bo FROM o GROUP BY kind$$) AS variance_exact;
SELECT same($$SELECT store, round(stddev_samp(f8)::numeric, 9) AS sd, round(corr(f8, f4)::numeric, 9) AS c, regr_count(f8, f4) AS n,
			  round(regr_slope(f8, f4)::numeric, 9) AS slope FROM o GROUP BY store$$) AS float_stats;
SELECT same($$SELECT kind, bool_and(price > 500) AS a, bool_or(tag IS NULL) AS b, every(store < 9) AS e, min(tag) AS lo, max(tag) AS hi, min(kind) AS lk FROM o GROUP BY kind$$) AS booleans_text;
-- expressions of several columns and of the unnested items
SELECT same($$SELECT kind, variance(price * store) AS v, max(upper(tag) || kind) AS m, min(price - store) AS d FROM o GROUP BY kind$$) AS expressions;
SELECT same($$SELECT i.category, round(stddev_samp(i.qty * i.price)::numeric, 9) AS sd, bit_or(i.qty) AS bo, string_agg(DISTINCT o.tag, '-') AS tags
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY i.category$$) AS items;
-- DISTINCT: btree order, the first of equal values, NULL last, a constant second argument
SELECT same($$SELECT kind, sum(DISTINCT store) AS s, avg(DISTINCT price) AS a, array_agg(DISTINCT tag) AS tags, string_agg(DISTINCT tag, ',') AS tg,
			  array_agg(DISTINCT day) AS days, count(DISTINCT f4) AS f FROM o GROUP BY kind$$) AS distinct_values;
SELECT same($$SELECT kind, min(DISTINCT price) AS lo, max(DISTINCT tag) AS hi, sum(DISTINCT store) FILTER (WHERE id < 300) AS sf FROM o GROUP BY kind$$) AS distinct_filter;
-- the rows arrive in scan order (the normal plan sorts first, so the collected values are compared sorted)
SELECT same($$SELECT kind, (SELECT array_agg(x ORDER BY x) FROM unnest(array_agg(id)) x) AS ids, (SELECT string_agg(x, ',' ORDER BY x) FROM unnest(array_agg(tag)) x) AS tags,
			  (SELECT jsonb_agg(x ORDER BY x) FROM unnest(array_agg(price)) x) AS js FROM o WHERE store < 3 GROUP BY kind$$) AS scan_order;
-- ordered-set aggregates: arrays of fractions, intervals, text, mode(), DESC, FILTER, a group without values
SELECT same($$SELECT kind, percentile_cont(ARRAY[0.1, 0.5, 0.9]) WITHIN GROUP (ORDER BY price::float8 / 100) AS arr,
			  percentile_cont(0.5) WITHIN GROUP (ORDER BY (day - date '2024-01-01') * interval '1 hour') AS iv,
			  percentile_disc(ARRAY[0, 0.5, 1]::float8[]) WITHIN GROUP (ORDER BY tag) AS tg,
			  mode() WITHIN GROUP (ORDER BY tag) AS m, mode() WITHIN GROUP (ORDER BY store) FILTER (WHERE price > 5000) AS m2,
			  percentile_disc(0.3) WITHIN GROUP (ORDER BY day DESC) AS d3, percentile_disc(0.5) WITHIN GROUP (ORDER BY price) FILTER (WHERE id < 0) AS none
			  FROM o GROUP BY kind$$) AS ordered_sets;
SELECT same($$SELECT kind, store, grouping(kind, store) AS g, stddev_pop(price) AS sd, mode() WITHIN GROUP (ORDER BY tag) AS m FROM o GROUP BY ROLLUP (kind, store)$$) AS rollup_ordered;
SELECT same($$SELECT stddev_samp(price) AS sd, string_agg(tag, ',') AS s, mode() WITHIN GROUP (ORDER BY tag) AS m, sum(f4) AS f, array_agg(day) AS d FROM o WHERE id < 0$$) AS no_rows;
-- user aggregates
SELECT same($$SELECT kind, round(sum_sq(f8)::numeric, 6) AS q, sum_sq(store) FILTER (WHERE tag IS NULL) AS n FROM o GROUP BY kind$$) AS user_aggregate;
-- memory: DISTINCT, ORDER BY and WITHIN GROUP inputs go through a tuplesort bounded by work_mem; a DISTINCT hash set that outgrows it moves there
SET work_mem = 64;
SET client_min_messages = warning;
SELECT kind, cardinality(array_agg(DISTINCT id)) AS ids, sum(DISTINCT id % 100) AS s FROM o GROUP BY kind ORDER BY kind;
RESET client_min_messages;
SELECT same($$SELECT kind, array_agg(DISTINCT id) AS ids, sum(DISTINCT id % 100) AS s, string_agg(DISTINCT tag, ',') AS tags FROM o GROUP BY kind$$) AS distinct_spill;
SELECT same($$SELECT kind, array_agg(id ORDER BY price, id) AS ids, regr_count(DISTINCT f8, price) AS pairs, percentile_disc(0.5) WITHIN GROUP (ORDER BY id) AS med FROM o GROUP BY kind$$) AS sorted_small_work_mem;
RESET work_mem;
-- errors are the aggregate's own: float4 overflow in the transition function, and the same row order
SELECT kind, sum(f4 * 1e38::float4) FROM o GROUP BY kind;
SET sift.enable_agg = off;
SELECT kind, sum(f4 * 1e38::float4) FROM o GROUP BY kind;
RESET sift.enable_agg;
-- privileges are ExecAgg's: EXECUTE on the aggregate for the user, the component functions are the aggregate owner's business
CREATE ROLE generic_reader;
GRANT USAGE ON SCHEMA sift TO generic_reader;
GRANT SELECT ON o TO generic_reader;
REVOKE EXECUTE ON FUNCTION sq_add(float8, float8) FROM PUBLIC;
SET ROLE generic_reader;
SELECT kind, round(sum_sq(f8)::numeric, 3) AS q FROM o GROUP BY kind ORDER BY kind;
RESET ROLE;
REVOKE EXECUTE ON FUNCTION sum_sq(float8) FROM PUBLIC;
SET ROLE generic_reader;
SELECT kind, round(sum_sq(f8)::numeric, 3) AS q FROM o GROUP BY kind ORDER BY kind;
RESET ROLE;
-- the shared database of a plain installcheck goes on with the next test
DROP OWNED BY generic_reader;
DROP ROLE generic_reader;
SET client_min_messages = warning;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m4 CASCADE;
-- <<< generic

-- >>> json
SET client_min_messages = warning;
CREATE SCHEMA m5;
GRANT USAGE ON SCHEMA m5 TO PUBLIC;
SET search_path = m5, public;
SET datestyle = 'ISO, MDY';
SET timezone = 'UTC';
-- the shapes: normal, empty, SQL NULL, JSON null, other JSON types, elements that are not objects, fields of other types
CREATE TEMP TABLE pat (k int, metrics jsonb, items jsonb, notes jsonb, details jsonb);
INSERT INTO pat VALUES
 (1, '{"total": 8.5, "shipping": 7, "m_tax": 9}',
	 '[{"category":"Garden","sku":"bulb","kind":"credit","qty":2,"price":8.2,"discount":0.1},{"category":"Home","sku":"lamp","kind":"debit","qty":1,"price":20}]',
	 '[{"kind":"gift","text":"for you"}]',
	 '{"lang":"en","flag_a":true,"nested":{"when":"2024-03-01","at":"2024-03-01 10:20:30","n":12345678901,"f":1.5}}'),
 (2, '{}', '[]', '[]', '{}'),
 (3, NULL, NULL, NULL, NULL),
 (4, 'null', 'null', 'null', 'null'),
 (5, '[1,2]', '{"a":1}', '"x"', '[1]'),
 (6, '"str"', '5', '5', '"text"'),
 (7, '5', '[1, "x", null, [1,2], {"sku":"a","qty":"7","price":"3.25"}, {}, {"category": null, "qty": null, "price": 1e2}]',
	 '[{"kind":"a"},{"kind":"b"},{"text":"c"}]',
	 '{"lang": 5, "flag_a": "true", "nested": {"when": "2024-12-31", "at": "2024-12-31 23:59:59.5", "n": "42", "f": "2.5"}}'),
 (8, '{"a": null, "b": "7.5", "c": 2, "d": "1e3", "zz": -0.5, "": 1.25, "ünï": 3}',
	 '[{"category":"Garden","sku":"x","kind":"credit","qty":-3,"price":0}]',
	 '[]',
	 '{"lang": {"x": [1, "b"]}, "flag_a": 0, "nested": [1]}'),
 (9, '{"x": {"y": 1}, "z": null}'::jsonb - 'x', '{"items": []}', '[{"kind": null, "text": null}]',
	 '{"lang": true, "flag_a": null, "nested": {"when": null, "at": null, "n": null, "f": null}}'),
 (10, '{"p": 1, "q": 2, "r": 3, "s": 4}', '[{"category":"Home","sku":"lamp","kind":"debit","qty":5,"price":0.25},{"category":"Home","sku":"lamp","kind":"debit","qty":6,"price":0.5},{"category":"Garden","sku":"seed","kind":"debit","qty":0,"price":3}]',
	 '[{"kind":"gift"}]', '{"lang": "de", "flag_a": "t", "nested": "gone"}');
-- about 1500 rows in several superblocks
CREATE TABLE o (id int, metrics jsonb, items jsonb, notes jsonb, details jsonb, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO o SELECT (g - 1) * 10 + p.k, p.metrics, p.items, p.notes, p.details, repeat('x', 200) FROM generate_series(1, 150) g, pat p ORDER BY g, p.k;
CREATE INDEX o_col ON o USING sift_col (id, metrics, items, notes, details)
	WITH (paths = 'metrics.* float8, items[]{category text, sku text, kind text, qty int4, price float8}, notes[]{kind text, text text},
	               details.lang text, details.flag_a bool, details.nested.n int8, details.nested.f float8');
VACUUM ANALYZE o;
-- scalar paths: -> then ->> and the cast of the declared type, over NULLs, JSON null, other JSON types and fields of other types
SELECT x.name, mig_same(format('SELECT count(*) AS n, count(v) AS c, min(v) AS lo, max(v) AS hi FROM (SELECT (%s)::text AS v FROM o) t', x.expr)) AS same
FROM (VALUES ('lang', $$details->>'lang'$$),
			 ('flag_a', $$substr(((details->>'flag_a')::bool)::text, 1, 1)$$),
			 ('nested.when', $$(details->'nested'->>'when')::date$$),
			 ('nested.at', $$(details->'nested'->>'at')::timestamp$$),
			 ('nested.n', $$(details->'nested'->>'n')::int8$$),
			 ('nested.f', $$(details->'nested'->>'f')::float8$$)) x(name, expr) ORDER BY x.name;
SELECT mig_same($$SELECT details->>'lang' AS lang, count(*) FROM o GROUP BY 1$$) AS lang;
SELECT mig_same($$SELECT details->>'flag_a' AS flag_a, count(*) FROM o GROUP BY 1$$) AS flag_a;
-- the free keys of an object: the count (NULL when the column is not an object), the keys and the values
SELECT mig_same($$SELECT CASE WHEN jsonb_typeof(metrics) = 'object' THEN (SELECT count(*) FROM jsonb_object_keys(metrics)) END AS keys, count(*) FROM o GROUP BY 1$$) AS counts;
SELECT mig_same($$SELECT e.key, count(*), sum((e.value #>> '{}')::float8) FROM o, jsonb_each(CASE WHEN jsonb_typeof(o.metrics) = 'object' THEN o.metrics END) e
				  WHERE jsonb_typeof(e.value) IN ('number', 'null') GROUP BY 1$$) AS keys_values;
-- an array of objects: the count (NULL: a missing key, JSON null or not an array), then the fields of the elements
SELECT mig_same($$SELECT CASE WHEN jsonb_typeof(items) = 'array' THEN jsonb_array_length(items) END AS elements, count(*) FROM o GROUP BY 1$$) AS counts;
SELECT mig_same($$SELECT e.v->>'category' AS category, e.v->>'kind' AS kind, count(*), sum((e.v->>'qty')::int4), sum((e.v->>'price')::float8)
				  FROM o, jsonb_array_elements(CASE WHEN jsonb_typeof(o.items) = 'array' THEN o.items END) e(v)
				  WHERE jsonb_typeof(e.v) = 'object' AND jsonb_typeof(e.v->'qty') IN ('number', 'null') AND jsonb_typeof(e.v->'price') IN ('number', 'null') GROUP BY 1, 2$$) AS fields;
SELECT mig_same($$SELECT e.v->>'kind' AS kind, count(*) FROM o, jsonb_array_elements(CASE WHEN jsonb_typeof(o.notes) = 'array' THEN o.notes END) e(v) GROUP BY 1$$) AS second_array;
-- values the cast refuses: the error is the SQL cast's
CREATE TABLE e (id int, j jsonb);
INSERT INTO e VALUES (1, '{"a": 7.5}'), (2, '{"a": true}'), (3, '{"a": "yes"}'), (4, '{"a": {"b": "x"}}'), (5, '[{"b": 1}, {"b": 2}, 3]');
CREATE INDEX e_col ON e USING sift_col (id, j) WITH (paths = 'j.a text');
SELECT x.name, mig_same(format('SELECT count(*), count(v) FROM (SELECT (%s) AS v FROM e) t', x.expr)) AS same
FROM (VALUES ('int4 of 7.5', $$(j #>> '{a}')::int4$$), ('float8 of true', $$(j #>> '{a}')::float8$$), ('bool of yes', $$(j #>> '{a}')::bool$$),
			 ('text of object', $$j #>> '{a}'$$), ('root array', $$j #>> '{0,b}'$$)) x(name, expr) ORDER BY x.name;
-- 255 bytes of text are the limit of a value, a key and a field of an element in the column index: longer ones read as the normal plan reads them
TRUNCATE e;
INSERT INTO e VALUES (1, jsonb_build_object('a', repeat('k', 255), 'l', jsonb_build_array(jsonb_build_object('t', repeat('é', 127))))),
					 (2, jsonb_build_object('a', repeat('k', 256))),
					 (3, jsonb_build_object('l', jsonb_build_array(jsonb_build_object('t', repeat('é', 128))))),
					 (4, jsonb_build_object(repeat('k', 256), 1));
SELECT mig_same($$SELECT length(j #>> '{a}') AS a, count(*) FROM e GROUP BY 1$$) AS long_value;
SELECT mig_same($$SELECT length(x.t) AS t, count(*) FROM e, jsonb_to_recordset(e.j -> 'l') AS x(t text) WHERE jsonb_typeof(e.j -> 'l') = 'array' GROUP BY 1$$) AS long_element;
SELECT mig_same($$SELECT count(*) AS keys, max(length(k)) AS longest FROM e, jsonb_object_keys(e.j) k WHERE jsonb_typeof(e.j) = 'object'$$) AS long_key;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m5 CASCADE;
-- <<< json

-- >>> lookup
SET client_min_messages = warning;
CREATE SCHEMA m6;
GRANT USAGE ON SCHEMA m6 TO PUBLIC;
SET search_path = m6, public;
-- Custom Scan (SiftAgg) over the test column source: lookup joins to small tables (duplicate, NULL and missing keys, a table that changes),
-- keys and filters over several leaves, a key of an object leaf read at the row level, and an outer aggregate over per-order aggregates.
-- The rows are the ones of the normal plan (sift.enable_agg off).
SET max_parallel_workers_per_gather = 0;
CREATE TABLE o (id int PRIMARY KEY, store int, kind text, metrics jsonb, items jsonb, pad text) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO o
SELECT g, g % 7, (ARRAY['web', 'shop', 'phone'])[1 + g % 3],
	   CASE g % 6 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN jsonb_build_object('value', g % 5) ELSE jsonb_build_object('m_total', 1 + (g % 40) / 10.0, 'value', g % 5) END,
	   CASE WHEN g % 19 = 0 THEN NULL ELSE jsonb_build_array(
			jsonb_build_object('category', (ARRAY['A', 'b', 'C', 'D', 'E'])[1 + g % 5], 'kind', CASE WHEN g % 4 = 0 THEN NULL ELSE (ARRAY['x', 'y'])[1 + g % 2] END, 'qty', g % 5, 'price', (g % 9) * 2.5),
			jsonb_build_object('category', (ARRAY['A', 'b', 'C', 'D', 'E'])[1 + (g / 3) % 5], 'kind', 'y', 'qty', g % 3, 'price', (g % 7) * 1.5)) END,
	   repeat('x', 100)
FROM generate_series(1, 1500) g;
CREATE INDEX o_col ON o USING sift_col (id, store, kind, metrics, items) WITH (paths = 'metrics.* float8, items[]{category text, kind text, qty int4, price float8}');
VACUUM ANALYZE o;
-- the mapping tables: a key with two rows, a NULL key, a key that differs by case, keys without a row (E, x)
CREATE TABLE cat_map (category text, flag_a text);
INSERT INTO cat_map VALUES ('A', 'a1'), ('A', 'a2'), ('B', 'bb'), ('b', 'lower'), ('C', NULL), (NULL, 'nothing'), ('D', 'dd');
ANALYZE cat_map;
CREATE TABLE empty_map (category text, flag_a text);
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_equals_normal(q) $$;
-- the plans
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
SELECT same($$SELECT i.category, count((o.metrics->>'m_total')::float8) AS c, round(avg((o.metrics->>'m_total')::float8)::numeric, 9) AS a, max((o.metrics->>'value')::float8) AS v
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY 1$$) AS metric_key;
SELECT same($$SELECT store, count(*) FILTER (WHERE (metrics->>'value')::float8 >= 2) AS good, round(avg((metrics->>'m_total')::float8)::numeric, 9) AS a
			  FROM o WHERE (metrics->>'m_total')::float8 IS NOT NULL GROUP BY 1$$) AS metric_key_rows;
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
-- semi and anti joins, a key that is an expression, the rest of an ON, and aggregates of other numeric types taken from PostgreSQL's own functions
CREATE TABLE num_map (category text, a2 int2, a4 int4, a8 int8, iv interval);
INSERT INTO num_map VALUES ('A', 1, 10, 100, '1 day'), ('A', 2, NULL, 200, '2 hours'), ('b', 3, 30, NULL, NULL), ('C', NULL, 40, 400, '3 days'), (NULL, 5, 50, 500, '5 days');
SELECT same($$SELECT i.category, count(*) AS n, sum(i.qty) AS q FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  WHERE EXISTS (SELECT 1 FROM cat_map m WHERE m.category = i.category) GROUP BY 1$$) AS semi;
SELECT same($$SELECT i.category, count(*) AS n FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  WHERE NOT EXISTS (SELECT 1 FROM cat_map m WHERE m.category = i.category) GROUP BY 1$$) AS anti;
SELECT same($$SELECT i.category, count(*) AS n FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  WHERE i.category IN (SELECT category FROM cat_map WHERE flag_a IS NOT NULL) GROUP BY 1$$) AS semi_in;
SELECT same($$SELECT m.flag_a, count(*) AS n FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  JOIN cat_map m ON m.category = upper(i.category) GROUP BY 1$$) AS key_expr;
SELECT same($$SELECT i.category, m.flag_a, count(*) AS n FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN cat_map m ON m.category = i.category AND m.flag_a <> 'a2' AND i.price > 2 GROUP BY 1, 2$$) AS on_filter;
SELECT same($$SELECT i.category, count(*) AS n FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN cat_map m ON m.category = i.category WHERE m.flag_a IS NULL GROUP BY 1$$) AS where_left;
SELECT same($$SELECT i.category, sum(m.a2) AS s2, avg(m.a4) AS v4, sum(m.a4) AS s4, sum(m.a8) AS s8, avg(m.a8) AS v8, sum(m.iv) AS si, avg(m.iv) AS vi
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) LEFT JOIN num_map m ON m.category = i.category GROUP BY 1$$) AS pg_aggregates;
-- the same from workers: the tables of the lookup joins, the combos and the members of a free-key object are merged by value, and the float sums are the leader's alone
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT same($$SELECT m.flag_a, count(*) AS n, sum(i.qty) AS q, count(DISTINCT o.id) AS orders
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  LEFT JOIN cat_map m ON m.category = i.category GROUP BY 1$$) AS left_join_workers;
SELECT same($$SELECT i.category, m.flag_a, count(*) AS n, sum(i.price) AS p
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
			  JOIN cat_map m ON m.category = i.category GROUP BY 1, 2$$) AS inner_join_workers;
SELECT same($$SELECT m.flag_a, count(*) AS n FROM o LEFT JOIN cat_map m ON m.category = o.kind GROUP BY 1$$) AS row_level_workers;
SELECT same($$SELECT CASE WHEN o.kind = 'web' THEN i.category ELSE coalesce(i.kind, '?') END AS pick, coalesce(o.kind, '') || '/' || (o.store % 2) AS k, count(*) AS n
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8)
			  WHERE o.kind = 'web' OR i.price > 10 GROUP BY 1, 2$$) AS several_leaves_workers;
SELECT same($$SELECT i.category, count((o.metrics->>'m_total')::float8) AS c, round(avg((o.metrics->>'m_total')::float8)::numeric, 9) AS a, max((o.metrics->>'value')::float8) AS v
			  FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY 1$$) AS metric_key_workers;
SELECT same($$SELECT category, avg(p) AS a, count(DISTINCT id) AS orders, sum(n) AS items, count(DISTINCT id) FILTER (WHERE p > 5) AS high
			  FROM (SELECT o.id, i.category, avg(i.price) AS p, count(*) AS n
					FROM o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8) GROUP BY o.id, i.category) x GROUP BY category$$) AS per_order_workers;
RESET max_parallel_workers_per_gather;
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m6 CASCADE;
-- <<< lookup

-- >>> scan
SET client_min_messages = warning;
CREATE SCHEMA m7;
GRANT USAGE ON SCHEMA m7 TO PUBLIC;
SET search_path = m7, public;
-- The scan of Custom Scan (SiftAgg) over the test column source: workers aggregate the superblocks of the table and the leader combines
-- their groups, units whose zone misses the id range of the query are skipped, count(DISTINCT) is counted per group. In every case the
-- rows are the ones of the normal plan (sift.enable_agg off), and float sums are the same whoever scans.
-- fillfactor 10: about six rows a page, five superblocks of 256 pages; id grows with the heap, so a unit holds a narrow range of ids
CREATE TABLE s (id int, g int, small int, wide int, a float8, b float8, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO s
SELECT i, i % 7, i % 50, (i * 7919) % 100003, (i % 64) / 4.0, (i % 1000) * 0.1, NULL
FROM generate_series(1, 8000) i;
CREATE INDEX s_col ON s USING sift_col (id, g, small, wide, a, b);
ANALYZE s;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
-- the normal plan of a table this small is cheaper than the scan: leave it only the plan of the reference
SET enable_seqscan = off;
-- a query's workers launched, units read and units pruned, from its EXPLAIN ANALYZE
-- rows that differ between the scan of the first setting (SiftAgg, no workers) and of the second, and the rows of the first
CREATE FUNCTION diffs(q text, agg2 bool, workers2 int, OUT differ bigint, OUT nrows bigint) LANGUAGE plpgsql AS $$
BEGIN
	SET sift.enable_agg = on;
	SET max_parallel_workers_per_gather = 0;
	EXECUTE format('CREATE TEMP TABLE r_a AS %s', q);
	PERFORM set_config('sift.enable_agg', agg2::text, false), set_config('max_parallel_workers_per_gather', workers2::text, false);
	EXECUTE format('CREATE TEMP TABLE r_b AS %s', q);
	SELECT count(*) INTO differ FROM ((TABLE r_a EXCEPT ALL TABLE r_b) UNION ALL (TABLE r_b EXCEPT ALL TABLE r_a)) d;
	SELECT count(*) INTO nrows FROM r_a;
	DROP TABLE r_a, r_b;
	RESET sift.enable_agg;
	RESET max_parallel_workers_per_gather;
END $$;
-- parallel: the plan, the workers that ran, and the same groups as the normal plan
SET max_parallel_workers_per_gather = 0;
SELECT * FROM diffs($$SELECT g, count(*) AS n, sum(small) AS sm, min(wide) AS lo, max(wide) AS hi, avg(small) AS av FROM s GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, count(*) AS n, sum(small) AS sm, min(wide) AS lo, max(wide) AS hi, avg(small) AS av FROM s GROUP BY g$$, true, 3);
SELECT * FROM diffs($$SELECT wide % 5003 AS k, count(*) AS n FROM s GROUP BY 1$$, true, 2);
-- float sums that depend on the order of the additions: the same bits with no worker and with any number of them
SELECT * FROM diffs($$SELECT g, round(sum(b)::numeric, 3) AS sb, round(avg(b)::numeric, 3) AS ab, round(sum(b * 1.0001)::numeric, 3) AS sx FROM s GROUP BY g$$, true, 2);
SELECT * FROM diffs($$SELECT g, round(sum(b)::numeric, 3) AS sb, round(avg(b)::numeric, 3) AS ab, round(sum(b * 1.0001)::numeric, 3) AS sx FROM s GROUP BY g$$, true, 4);
SELECT * FROM diffs($$SELECT round(sum(b)::numeric, 3) AS sb FROM s$$, true, 3);
-- and the normal plan agrees for sums that do not depend on it
SELECT * FROM diffs($$SELECT g, sum(a) AS sa, avg(a) AS aa FROM s GROUP BY g$$, false, 0);
-- zone pruning: units are skipped by the range of ids of the conjuncts, with and without workers, and the groups are the same
SELECT * FROM diffs($$SELECT g, count(*) AS n, sum(small) AS sm FROM s WHERE id BETWEEN 3000 AND 3200 GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, count(*) AS n FROM s WHERE id = 7000 GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, count(*) AS n FROM s WHERE id > 7000 AND id <= 7500 AND g > 1 GROUP BY g$$, true, 2);
SELECT * FROM diffs($$SELECT g, count(*) AS n FROM s WHERE id >= 100000 GROUP BY g$$, false, 0);
-- count(DISTINCT): few values (a bit per value) and many (a hash set), per group and in total, in one process and merged from several
SELECT * FROM diffs($$SELECT g, count(DISTINCT small) AS d, count(DISTINCT wide) AS w, count(*) AS n FROM s GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, count(DISTINCT small) AS d, count(DISTINCT wide) AS w, count(*) AS n FROM s GROUP BY g$$, true, 3);
SELECT * FROM diffs($$SELECT count(DISTINCT small) AS d, count(DISTINCT wide) AS w FROM s$$, true, 2);
SELECT * FROM diffs($$SELECT g, count(DISTINCT wide) FILTER (WHERE id % 3 = 0) AS d FROM s WHERE id > 2000 GROUP BY g$$, true, 2);
-- ordered sets (the histograms are merged by value), numeric sums and keys, grouping sets and float keys come from the workers as the other kinds do
SET max_parallel_workers_per_gather = 2;
RESET max_parallel_workers_per_gather;
SELECT * FROM diffs($$SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY small) AS m, count(*) AS n FROM s GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY small) AS m, percentile_cont(0.9) WITHIN GROUP (ORDER BY small) AS c, percentile_cont(0.25) WITHIN GROUP (ORDER BY b) AS cb, count(*) AS n FROM s GROUP BY g$$, true, 3);
SELECT * FROM diffs($$SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY b) AS cb FROM s$$, true, 2);
-- a histogram that does not fit work_mem is a sort of the pairs, in the workers and in the leader that merges them
SET work_mem = '64kB';
SELECT * FROM diffs($$SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY wide) AS m, percentile_cont(0.9) WITHIN GROUP (ORDER BY wide) AS c FROM s GROUP BY g$$, true, 3);
RESET work_mem;
SELECT * FROM diffs($$SELECT g, small % 3 AS k, count(*) AS n FROM s GROUP BY GROUPING SETS ((g), (small % 3), ())$$, false, 0);
SELECT * FROM diffs($$SELECT g, small % 3 AS k, count(*) AS n, round(sum(b)::numeric, 3) AS sb FROM s GROUP BY GROUPING SETS ((g), (small % 3), ())$$, true, 3);
SELECT * FROM diffs($$SELECT g, sum(small::numeric) AS sm, avg(small::numeric) AS av FROM s GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, sum(small::numeric) AS sm, avg(small::numeric) AS av, min(small::numeric * 1.5) AS lo, max(small::numeric * 1.5) AS hi FROM s GROUP BY g$$, true, 3);
SELECT * FROM diffs($$SELECT small::numeric * g::numeric AS k, count(*) AS n, round(sum(b)::numeric, 3) AS sb FROM s GROUP BY 1$$, true, 2);
SELECT * FROM diffs($$SELECT a, count(*) AS n, round(sum(b)::numeric, 3) AS sb FROM s GROUP BY a$$, true, 3);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m7 CASCADE;
-- <<< scan

-- >>> sets
SET client_min_messages = warning;
CREATE SCHEMA m8;
GRANT USAGE ON SCHEMA m8 TO PUBLIC;
SET search_path = m8, public;
-- Custom Scan (SiftAgg) over the test column source: grouping sets (GROUPING(), the empty set, NULL data against rollup NULLs), a WHERE on
-- the outer query of per-order aggregates, aggregates of the columns of a lookup join and float group keys (-0, 0 and NaN: the stub
-- keeps the values of the heap). The rows are the ones of the normal plan (sift.enable_agg off).
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
CREATE INDEX o_col ON o USING sift_col (id, store, kind, f8, f4, items) WITH (paths = 'items[]{category text, kind text, qty int4, price float8}');
VACUUM ANALYZE o;
-- the values to aggregate through a lookup join: a key with two rows, NULL values, a key without a row (E)
CREATE TABLE vals (category text, label text, weight numeric(8,2), factor float8);
INSERT INTO vals VALUES ('A', 'a1', 1.50, 0.5), ('A', 'a2', 2.25, NULL), ('b', NULL, 3.10, 2.0), ('C', 'cc', NULL, 1.5), ('D', 'dd', 0.05, 0.25);
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_equals_normal(q) $$;
-- the plans
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
CREATE INDEX dd_col ON dd USING sift_col (id, grp, dense, wide, few);
VACUUM ANALYZE dd;
SET parallel_leader_participation = off;
SELECT same($$SELECT grp, count(DISTINCT dense) AS dd, count(DISTINCT wide) AS dw, count(DISTINCT few) AS df, count(DISTINCT id) AS di FROM dd GROUP BY grp$$) AS distinct_bitmaps_workers;
SELECT same($$SELECT count(DISTINCT dense) AS dd, count(DISTINCT wide) AS dw, count(DISTINCT few) AS df FROM dd$$) AS distinct_global_workers;
SELECT same($$SELECT grp, count(DISTINCT dense * 100003) AS de FROM dd GROUP BY grp$$) AS distinct_expr_workers;
SET work_mem = '64kB';
SET hash_mem_multiplier = 8;
SELECT same($$SELECT grp, count(DISTINCT dense) AS dd, count(DISTINCT wide) AS dw FROM dd GROUP BY grp$$) AS distinct_spilled_workers;
RESET hash_mem_multiplier;
RESET work_mem;
RESET parallel_leader_participation;
RESET max_parallel_workers_per_gather;
-- arms of one UNION ALL that scan the same rows for the same aggregates share one pass; the rows are the ones of the arms alone
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
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA m8 CASCADE;
-- <<< sets

DROP FUNCTION mig_same(text), mig_run(text, bool);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;

