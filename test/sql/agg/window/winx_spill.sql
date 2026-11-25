-- A peer group of more rows than work_mem holds no longer ends a V6 RANGE window with ERROR 54000: the rows past max_pending go to a temp file and come back, in order, when the peer group
-- closes. The plan is made at the default work_mem and the statement runs at 64kB (max_pending = 2048), with a peer group of 10000 tied rows over integer, numeric, float8 and text columns: it answers
-- as the heap does. A LIMIT over the window and an error raised while the spill is open leave no temp file behind. Every check prints t.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
	IF current_setting('sift.agg_force', true) IS NOT NULL THEN
		PERFORM set_config('sift.agg_force', 'on', false);
	END IF;
END $$;
SET sift.enable_agg = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE sh (id int PRIMARY KEY, p int NOT NULL, grp int NOT NULL, v int, n numeric(12,2), f float8, s text) WITH (autovacuum_enabled = false);
INSERT INTO sh
SELECT i, CASE WHEN i <= 12000 THEN 1 ELSE 2 END,
	CASE WHEN i <= 1500 THEN 1 WHEN i <= 11500 THEN 2 WHEN i <= 12000 THEN 3 ELSE 4 END,
	CASE WHEN i % 17 = 0 THEN NULL ELSE i % 1000 END,
	CASE WHEN i % 19 = 0 THEN NULL ELSE (i * 37 % 5000) / 8.0 END,
	CASE WHEN i % 23 = 0 THEN NULL ELSE (i % 301) / 4.0 END,
	repeat(chr(97 + i % 26), 1 + i % 40) || i
FROM generate_series(1, 12500) i;
CREATE TABLE sr (LIKE sh INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO sr SELECT * FROM sh;
CREATE INDEX sr_sift ON sr USING sift_col (p, grp, id, v, n, f, s);
VACUUM (FREEZE, ANALYZE) sh, sr;

CREATE FUNCTION rows_text(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE x text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', q) INTO x;
	RETURN x;
END $$;

-- the statement planned at the default work_mem (the plan is cached) and run at work_mem = wm, its rows in spill_out; the text of the rows, or of what post makes of them
CREATE FUNCTION spilled_text(q text, wm text DEFAULT '64kB', post text DEFAULT NULL) RETURNS text LANGUAGE plpgsql AS $$
DECLARE x text;
BEGIN
	DEALLOCATE ALL;
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	EXECUTE format('PREPARE spill_q AS %s', CASE WHEN post IS NULL THEN format('SELECT r::text AS r FROM (%s) r', q) ELSE q END);
	EXECUTE 'CREATE TEMP TABLE spill_warm ON COMMIT DROP AS EXECUTE spill_q';
	PERFORM set_config('work_mem', wm, true);
	EXECUTE 'CREATE TEMP TABLE spill_out ON COMMIT DROP AS EXECUTE spill_q';
	EXECUTE coalesce(post, 'SELECT coalesce(string_agg(r, E''\n'' ORDER BY r), '''') FROM spill_out') INTO x;
	DROP TABLE spill_warm, spill_out;
	DEALLOCATE spill_q;
	RETURN x;
END $$;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT rows_text(replace(q, '@T', 'sh')) = spilled_text(replace(q, '@T', 'sr')) $$;

CREATE FUNCTION plan_ok(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'sr') LOOP n := n + (line LIKE '%SiftAggV6Window%')::int; END LOOP;
	RETURN n = 1;
END $$;

CREATE FUNCTION tmp_files() RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FROM pg_ls_tmpdir() $$;

SELECT plan_ok('SELECT id, sum(n) OVER (PARTITION BY p ORDER BY grp) FROM @T') AS sum_plan;
SELECT plan_ok('SELECT id, count(*) FILTER (WHERE v < 500) OVER (PARTITION BY p ORDER BY grp) FROM @T') AS filter_plan;
SELECT parity('SELECT id, grp, s, sum(v) FILTER (WHERE v >= 0) OVER w, count(*) OVER w, rank() OVER w, sum(n) OVER w FROM @T WINDOW w AS (PARTITION BY p ORDER BY grp)') AS int_sum_count_rank;
SELECT parity('SELECT id, s, sum(n) OVER w, avg(n) OVER w, min(n) OVER w, max(n) OVER w, count(n) OVER w FROM @T WINDOW w AS (PARTITION BY p ORDER BY grp)') AS numeric_aggs;
SELECT parity('SELECT id, s, sum(n) OVER w, dense_rank() OVER w, count(v) OVER w, min(v) OVER w, max(v) OVER w FROM @T WINDOW w AS (PARTITION BY p ORDER BY grp DESC)') AS desc_order;
SELECT parity('SELECT id, s, v, n, sum(v) OVER w, sum(n) OVER w FROM @T WINDOW w AS (PARTITION BY p ORDER BY grp RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)') AS range_frame;
SELECT parity('SELECT id, sum(v) FILTER (WHERE n > 100) OVER w, count(*) FILTER (WHERE v < 500) OVER w, sum(f) OVER w, avg(f) OVER w FROM @T WINDOW w AS (PARTITION BY p ORDER BY grp)') AS filtered;
SELECT parity('SELECT id, s, sum(n) OVER w, var_pop(v) OVER w, count(*) FILTER (WHERE s > ''m'') OVER w FROM @T WINDOW w AS (PARTITION BY p ORDER BY grp)') AS variances;

-- the tied rows come in no order the heap shares, so row_number, lag, lead and first_value are compared by what no order changes: per group, the numbers 1..n, the one row that lags or leads the neighbor group, the first group
SELECT rows_text('SELECT grp, count(*), min(rn), max(rn), sum(rn), count(lg), sum(lg), count(ld), sum(ld), min(fv), max(fv), sum(sm) FROM (SELECT grp, row_number() OVER w2 rn, lag(grp) OVER w2 lg, lead(grp) OVER w2 ld,
	first_value(grp) OVER w2 fv, sum(n) OVER w1 sm FROM sh WINDOW w1 AS (PARTITION BY p ORDER BY grp), w2 AS (PARTITION BY p ORDER BY grp, id)) w GROUP BY grp') =
	spilled_text('SELECT grp, row_number() OVER w AS rn, lag(grp) OVER w AS lg, lead(grp) OVER w AS ld, first_value(grp) OVER w AS fv, sum(n) OVER w AS sm FROM sr WINDOW w AS (PARTITION BY p ORDER BY grp)', '64kB',
	'SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (SELECT grp, count(*), min(rn), max(rn), sum(rn), count(lg), sum(lg), count(ld), sum(ld), min(fv), max(fv), sum(sm) FROM spill_out GROUP BY grp) r') AS order_free;

-- LIMIT ends the read of the window early; the spill is gone with the window
SELECT count(*) AS limit_rows FROM (SELECT id, sum(n) OVER (PARTITION BY p ORDER BY grp) FROM sr LIMIT 10) l;
SELECT tmp_files() AS tmp_after_limit;

-- a float8 whose square overflows in the 9500th row of the group of 10000 raises while the spill is open: the temp file goes with the subtransaction
UPDATE sr SET f = 1e200 WHERE id = 11000;
DO $$
BEGIN
	PERFORM spilled_text('SELECT id, var_pop(f) OVER (PARTITION BY p ORDER BY grp), count(*) OVER (PARTITION BY p ORDER BY grp) FROM sr');
	RAISE EXCEPTION 'the variance was to overflow';
EXCEPTION WHEN numeric_value_out_of_range THEN
	RAISE NOTICE 'overflow';
END $$;
DEALLOCATE ALL;
SELECT tmp_files() AS tmp_after_error;
DROP TABLE sh, sr;
DROP FUNCTION tmp_files(), plan_ok(text), parity(text), spilled_text(text, text, text), rows_text(text);
DROP EXTENSION pg_sift;
