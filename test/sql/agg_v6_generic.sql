-- Generic and ordered-set aggregates in SiftAggV6 (src/agg/v6/v6_gen.c): PostgreSQL's own transition, combine and final functions run over the values the scan decodes, with DISTINCT, ORDER BY and WITHIN GROUP
-- inputs sorted by one tuplesort, held to the answer of the heap for the same rows. Each parity check prints t; plan_ok prints t when the statement ran as SiftAggV6 over the index and over the table.
-- The statements hold on the release library too, whose indexes have the one format.
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET timezone = 'UTC';
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE gh (id int PRIMARY KEY, g int, a int, b numeric(12,2), f float8, t text, d date) WITH (autovacuum_enabled = false);
INSERT INTO gh
SELECT i, i % 7, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 37) % 101 END, (i % 50) / 4.0, (i % 13) / 3.0, CASE WHEN i % 11 = 0 THEN NULL ELSE 'k' || (i % 5) END, DATE '2024-01-01' + (i % 200)
FROM generate_series(1, 3000) i;
CREATE TABLE gc (LIKE gh INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO gc SELECT * FROM gh;
CREATE INDEX gc_col ON gc USING sift_col (id, g, a, b, f, t, d);
CREATE TABLE gt (LIKE gh INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO gt SELECT * FROM gh;
SELECT sift.table_maintain('gt') IS NOT NULL AS maintained;

CREATE AGGREGATE sum_sq (float8) (sfunc = float8pl, stype = float8, initcond = '0');
CREATE FUNCTION sq_add(s float8, x float8) RETURNS float8 LANGUAGE sql IMMUTABLE AS 'SELECT s + x * x';
CREATE AGGREGATE sum_sq2 (float8) (sfunc = sq_add, stype = float8, initcond = '0');

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;

-- @T is the heap twin, the sift_col index or the sift_table
CREATE FUNCTION parity(q text, with_table boolean DEFAULT true) RETURNS boolean LANGUAGE sql AS
$$ SELECT same_rows(replace(q, '@T', 'gc'), replace(q, '@T', 'gh')) AND (NOT with_table OR same_rows(replace(q, '@T', 'gt'), replace(q, '@T', 'gh'))) $$;

CREATE FUNCTION plan_ok(q text, with_table boolean DEFAULT true) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'gc') LOOP n := n + (line LIKE '%SiftAggV6%')::int; END LOOP;
	IF with_table THEN
		FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'gt') LOOP n := n + (line LIKE '%SiftAggV6%')::int; END LOOP;
	END IF;
	RETURN n = 1 + with_table::int;
END $$;

-- string_agg, array_agg, json_agg, jsonb_agg and json_object_agg, with ORDER BY and DISTINCT
SELECT parity('SELECT g, string_agg(t, '','' ORDER BY id), array_agg(a ORDER BY id) FROM @T GROUP BY g', false) AS string_array_ordered;
SELECT parity('SELECT g, string_agg(DISTINCT t, '','' ORDER BY t DESC), array_agg(DISTINCT a ORDER BY a) FROM @T GROUP BY g', false) AS distinct_ordered;
SELECT parity('SELECT g, jsonb_agg(d ORDER BY id), json_agg(b ORDER BY id) FROM @T WHERE id < 300 GROUP BY g') AS json_agg;
SELECT parity('SELECT g, json_object_agg(id, b ORDER BY id), jsonb_object_agg(id, d) FROM @T WHERE id < 200 GROUP BY g') AS object_agg;
SELECT plan_ok('SELECT g, string_agg(t, '','' ORDER BY id), array_agg(a ORDER BY id) FROM @T GROUP BY g', false) AS string_array_plan;
SELECT plan_ok('SELECT g, jsonb_agg(d ORDER BY id), json_agg(b ORDER BY id) FROM @T WHERE id < 300 GROUP BY g', false) AS json_plan;
-- the order of the scan, no ORDER BY: the outer query sorts what they collect
SELECT parity('SELECT g, (SELECT array_agg(v ORDER BY v) FROM unnest(arr) v) FROM (SELECT g, array_agg(a) AS arr FROM @T GROUP BY g) s') AS array_agg_unordered;
-- bit, regression and statistics aggregates, a user aggregate, a FILTER, an argument that is an expression
SELECT parity('SELECT g, bit_and(a), bit_or(a), bit_xor(a), bool_and(a > 3), bool_or(f > 3) FROM @T GROUP BY g') AS bit_bool;
SELECT parity('SELECT g, round(corr(b, f)::numeric, 9), round(covar_pop(b, f)::numeric, 9), round(regr_slope(b, f)::numeric, 9), regr_count(b, f), round(stddev(b), 9), round(var_pop(a), 9) FROM @T GROUP BY g') AS regr_stats;
SELECT parity('SELECT g, round(sum_sq(f)::numeric, 6), round(sum_sq2(f)::numeric, 6), round(sum_sq2(b::float8) FILTER (WHERE a > 50)::numeric, 6) FROM @T GROUP BY g') AS user_aggregate;
SELECT parity('SELECT sum_sq2(f), count(*), array_agg(a) FILTER (WHERE a > 90) IS NOT NULL FROM @T') AS ungrouped;
SELECT parity('SELECT g, max(a * 2 + 1), string_agg((a % 3)::text, '''' ORDER BY id) FROM @T GROUP BY g', false) AS expression_argument;
SELECT plan_ok('SELECT g, bit_or(a), round(corr(b, f)::numeric, 9), round(sum_sq2(f)::numeric, 6) FROM @T GROUP BY g', false) AS bit_regr_user_plan;
SELECT plan_ok('SELECT g, bit_or(a), sum_sq2(f) FROM @T GROUP BY g') AS bit_user_table_plan;
-- DISTINCT of numeric and of several columns
SELECT parity('SELECT g, sum(DISTINCT b), avg(DISTINCT b), count(DISTINCT a), min(DISTINCT b), round(corr(DISTINCT b, f)::numeric, 9) FROM @T GROUP BY g') AS distinct_numeric;
-- ordered-set and hypothetical-set aggregates
SELECT parity('SELECT g, mode() WITHIN GROUP (ORDER BY t), mode() WITHIN GROUP (ORDER BY a) FROM @T GROUP BY g', false) AS mode;
SELECT parity('SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY a), percentile_disc(ARRAY[0.1, 0.9]) WITHIN GROUP (ORDER BY d), percentile_disc(0.3) WITHIN GROUP (ORDER BY b DESC) FROM @T GROUP BY g') AS percentile_disc;
SELECT parity('SELECT g, percentile_cont(0.5) WITHIN GROUP (ORDER BY f), percentile_cont(ARRAY[0.25, 0.5, 0.75]) WITHIN GROUP (ORDER BY f) FROM @T GROUP BY g') AS percentile_cont;
SELECT parity('SELECT g, rank(20) WITHIN GROUP (ORDER BY a), dense_rank(20) WITHIN GROUP (ORDER BY a), round(percent_rank(20) WITHIN GROUP (ORDER BY a)::numeric, 9), round(cume_dist(20) WITHIN GROUP (ORDER BY a)::numeric, 9) FROM @T GROUP BY g') AS hypothetical_set;
SELECT parity('SELECT rank(''k2'') WITHIN GROUP (ORDER BY t), mode() WITHIN GROUP (ORDER BY a) FILTER (WHERE g = 3) FROM @T', false) AS ungrouped_ordered;
SELECT plan_ok('SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY a), rank(20) WITHIN GROUP (ORDER BY a), mode() WITHIN GROUP (ORDER BY b) FROM @T GROUP BY g') AS ordered_set_plan;
-- no rows, and groups of NULLs only
SELECT parity('SELECT g, array_agg(a ORDER BY id), string_agg(t, '','' ORDER BY id), percentile_disc(0.5) WITHIN GROUP (ORDER BY a), rank(1) WITHIN GROUP (ORDER BY a) FROM @T WHERE id < 0 GROUP BY g') AS empty_grouped;
SELECT parity('SELECT array_agg(a ORDER BY id), string_agg(t, '','' ORDER BY id), percentile_disc(0.5) WITHIN GROUP (ORDER BY a), rank(1) WITHIN GROUP (ORDER BY a), bit_or(a) FROM @T WHERE id < 0') AS empty_ungrouped;
SELECT parity('SELECT g, array_agg(a), bit_or(a), mode() WITHIN GROUP (ORDER BY a) FROM @T WHERE a IS NULL GROUP BY g') AS nulls_only;
-- ROLLUP and several sets
SELECT parity('SELECT g, string_agg(DISTINCT t, '','' ORDER BY t), percentile_disc(0.5) WITHIN GROUP (ORDER BY a) FROM @T GROUP BY ROLLUP (g)', false) AS rollup;

-- workers: aggregates with a combine function keep a partial state in each worker (the serial and deserial functions of an internal one carry it); the rest stay serial
INSERT INTO gh SELECT i, i % 37, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 7919) % 1000 END, ((i * 31) % 10000) / 100.0, (i % 977) / 7.0, 'k' || (i % 53), DATE '2024-01-01' + (i % 200) FROM generate_series(3001, 70000) i;
INSERT INTO gc SELECT * FROM gh WHERE id > 3000;
CREATE FUNCTION par_ok(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; v6 boolean := false; par boolean := false;
BEGIN
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'gc') LOOP
		v6 := v6 OR line LIKE '%SiftAggV6%';
		par := par OR line LIKE '%Workers Planned%';
	END LOOP;
	RETURN v6 AND par;
END $$;
CREATE FUNCTION par_same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	PERFORM set_config('parallel_setup_cost', '0', true);
	PERFORM set_config('parallel_tuple_cost', '0', true);
	PERFORM set_config('min_parallel_table_scan_size', '0', true);
	RETURN same_rows(replace(q, '@T', 'gc'), replace(q, '@T', 'gh'));
END $$;
SELECT par_ok('SELECT g, bit_or(a), bit_xor(a), regr_count(b, f), round(stddev_samp(b), 6) FROM @T GROUP BY g') AS combine_plan;
SELECT par_same('SELECT g, bit_or(a), bit_xor(a), bit_and(a), regr_count(b, f), round(stddev_samp(b), 6), round(var_pop(a), 6) FROM @T GROUP BY g') AS combine_grouped;
SELECT par_same('SELECT bit_or(a), bit_xor(a), round(stddev(b), 6), round(sum_sq2(f)::numeric, 3) FROM @T') AS combine_ungrouped;
SELECT NOT par_ok('SELECT g, array_agg(a), string_agg(t, '','' ORDER BY id) FROM @T GROUP BY g') AS no_workers_for_ordered_or_growing;
SELECT par_same('SELECT g, array_length(array_agg(a), 1), length(string_agg(t, '','' ORDER BY id)), percentile_disc(0.5) WITHIN GROUP (ORDER BY a) FROM @T GROUP BY g') AS serial_still_exact;

DROP FUNCTION parity(text, boolean), plan_ok(text, boolean), par_ok(text), par_same(text), same_rows(text, text);
DROP AGGREGATE sum_sq (float8);
DROP AGGREGATE sum_sq2 (float8);
DROP FUNCTION sq_add(float8, float8);
DROP TABLE gt, gc, gh;
DROP EXTENSION pg_sift;
