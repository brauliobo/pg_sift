-- SiftAgg over the wide leaves of a sift_col index: numeric of more than 18 digits (two lanes) or of any other shape
-- (a canonical image), money and interval. A conjunct of the query on one of them is tested by the column engine on its stored lanes; every
-- other use of their values (a key, an aggregate, an OR, a FILTER, an expression) reads them as a dictionary of the unit's distinct values
-- (col_agg_wide_keys). Every query runs with and without the pushdown and the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;
SET lc_monetary = 'C';

CREATE TABLE wa (id int, grp int, qty int, n38 numeric(38, 10), n19 numeric(19, 2), img numeric, neg numeric(5, -2), m money, iv interval)
	WITH (autovacuum_enabled = false);
INSERT INTO wa
SELECT i, i % 7, i % 100,
	   CASE WHEN i % 13 = 0 THEN NULL WHEN i % 17 = 0 THEN 'NaN' WHEN i % 19 = 0 THEN 9999999999999999999999999999.9999999999
			WHEN i % 29 = 0 THEN -9999999999999999999999999999.9999999999 WHEN i % 23 = 0 THEN 0
			WHEN i % 2 = 0 THEN round(((i * 7919) % 2000001 - 1000000) / 100.0, 10)
			ELSE round(mod(i::numeric * 1234567890123456789012345.6789012345, 9999999999999999999999999999) * CASE WHEN i % 3 = 0 THEN -1 ELSE 1 END, 10) END,
	   CASE WHEN i % 11 = 0 THEN NULL WHEN i % 17 = 0 THEN 'NaN' WHEN i % 31 = 0 THEN 99999999999999999.99
			WHEN i % 37 = 0 THEN -99999999999999999.99 ELSE ((i * 104729) % 2000001 - 1000000) / 100.0 END,
	   CASE WHEN i % 10 = 0 THEN NULL WHEN i % 41 = 0 THEN 'NaN' WHEN i % 43 = 0 THEN 'Infinity' WHEN i % 47 = 0 THEN '-Infinity'
			WHEN i % 53 = 0 THEN 10::numeric ^ 400 + i WHEN i % 59 = 0 THEN -(10::numeric ^ 400) WHEN i % 61 = 0 THEN 1 / 10::numeric ^ 300
			WHEN i % 5 = 0 THEN (ARRAY[1.0, 1.00, 1, 1.000, 1.5, 1.50])[1 + i % 6]
			ELSE round(((i * 7919) % 200001 - 100000) / 7.0, 1 + i % 5) END,
	   CASE WHEN i % 6 = 0 THEN NULL ELSE ((i % 300) - 150) * 100 END,
	   CASE WHEN i % 9 = 0 THEN NULL WHEN i % 101 = 0 THEN '92233720368547758.07'::money WHEN i % 103 = 0 THEN '-92233720368547758.08'::money
			ELSE (((i * 7919) % 200001 - 100000)::numeric / 100)::money END,
	   CASE WHEN i % 12 = 0 THEN NULL WHEN i % 71 = 0 THEN 'infinity' WHEN i % 73 = 0 THEN '-infinity'
			WHEN i % 79 = 0 THEN '2147483647 months 2147483647 days 9223372036854775807 us'::interval
			WHEN i % 83 = 0 THEN '-2147483648 months -2147483648 days -9223372036854775808 us'::interval
			ELSE (ARRAY['1 month', '30 days', '720 hours', '1 mon 1 day', '31 days', '1 day 1 hour', '25 hours', '-1 month', '-30 days', '0',
						'00:00:00.000001', '-00:00:00.000001', '1 year', '12 mons', '365 days', '2 mons', '59 days', '60 days'])[1 + i % 18]::interval END
FROM generate_series(1, 20000) i;
CREATE INDEX wa_col ON wa USING sift_col (id, grp, qty, n38, n19, img, neg, m, iv);
VACUUM (FREEZE, ANALYZE) wa;

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

-- the wide conjuncts the source applies (0 when the plan has no SiftAgg), and whether the plan is the scan at all
CREATE FUNCTION wide_in_source(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '(\d+) wide filters in source' THEN n := substring(l from '(\d+) wide filters in source')::int; END IF;
	END LOOP;
	RETURN n;
END $$;
CREATE FUNCTION scanned(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- which shapes are pushed
SELECT wide_in_source($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 1234.5$$) AS numeric38,
       wide_in_source($$SELECT count(*), sum(qty) FROM wa WHERE n19 <= 99999999999999999.99 AND n38 <> 0$$) AS two,
       wide_in_source($$SELECT count(*), sum(qty) FROM wa WHERE img >= 1.5$$) AS image,
       wide_in_source($$SELECT count(*), sum(qty) FROM wa WHERE neg < 100$$) AS image_negative_scale,
       wide_in_source($$SELECT count(*), sum(qty) FROM wa WHERE m > '123.45'$$) AS money,
       wide_in_source($$SELECT count(*), sum(qty) FROM wa WHERE iv >= interval '1 month'$$) AS interval,
       wide_in_source($$SELECT count(*), sum(qty) FROM wa WHERE n38 IN (0, 1, 2) AND m NOT IN ('1.00', '2.00') AND iv IS NOT NULL$$) AS lists_and_null,
       wide_in_source($$SELECT grp, count(*) FROM wa WHERE n38 > 5 AND grp < 4 GROUP BY grp$$) AS grouped_by_other;
-- the rest is scanned too: OR, a key, an aggregate, an expression and a FILTER read the values
SELECT scanned($$SELECT count(*) FROM wa WHERE n38 > 1234.5 OR grp = 1$$) AS or_,
       scanned($$SELECT n19, count(*) FROM wa GROUP BY n19$$) AS key_,
       scanned($$SELECT sum(n38) FROM wa$$) AS sum_,
       scanned($$SELECT min(m), max(m) FROM wa$$) AS minmax_,
       scanned($$SELECT max(iv) FROM wa$$) AS interval_,
       scanned($$SELECT count(*) FILTER (WHERE m > '1.00') FROM wa$$) AS filter_,
       scanned($$SELECT count(*) FROM wa WHERE n38 + 1 > 5$$) AS expression_,
       scanned($$SELECT count(*) FROM wa WHERE img::text = '1'$$) AS cast_;

-- the cases are not empty: rows that pass, per trap
SELECT count(*) FILTER (WHERE n38 = 'NaN') AS nan38, count(*) FILTER (WHERE n38 IS NULL) AS null38, count(*) FILTER (WHERE n38 = 0) AS zero38,
       count(*) FILTER (WHERE n38 >= 9999999999999999999999999999.9999999999) AS max38, count(*) FILTER (WHERE img = 1) AS one_image,
       count(DISTINCT img::text) FILTER (WHERE img = 1) AS one_in_many_scales, count(*) FILTER (WHERE img = 'Infinity') AS inf_image,
       count(*) FILTER (WHERE m = '92233720368547758.07') AS max_money, count(*) FILTER (WHERE iv = interval '1 month') AS month_ties,
       count(DISTINCT iv::text) FILTER (WHERE iv = interval '1 month') AS month_spellings, count(*) FILTER (WHERE iv = 'infinity') AS inf_interval
FROM wa;

-- numeric(38, 10): every comparison, both ends of the range, constants of other scales and beyond the range, a NaN row greater than the rest
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 1234.5$$) AS gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 >= 1234.5$$) AS ge,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 < -1234.5$$) AS lt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <= -1234.5$$) AS le,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 = 0$$) AS eq,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <> 0$$) AS ne,
       same($$SELECT count(*), sum(qty) FROM wa WHERE 1234.5 < n38$$) AS commuted,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 = 9999999999999999999999999999.9999999999$$) AS max_eq,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 >= -9999999999999999999999999999.9999999999$$) AS min_ge,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 9999999999999999999999999999.9999999998$$) AS near_max;
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n38 = 1234.56789012345$$) AS finer_scale_eq,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <> 1234.56789012345$$) AS finer_scale_ne,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 0.00000000005 AND n38 < 0.00000000015$$) AS finer_scale_range,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 >= -0.00000000005 AND n38 <= 0.00000000005$$) AS finer_around_zero,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 < 1e60$$) AS far_above_lt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 1e60$$) AS far_above_gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > -1e60$$) AS far_below_gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <= -1e60$$) AS far_below_le,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 = 1e60$$) AS far_above_eq,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <> 1e60$$) AS far_above_ne;
-- NaN and the infinities as constants (numeric(38, 10) holds no infinity, a constant may be one)
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n38 = 'NaN'$$) AS nan_eq,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <> 'NaN'$$) AS nan_ne,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 < 'NaN'$$) AS nan_lt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <= 'NaN'$$) AS nan_le,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 'NaN'$$) AS nan_gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 >= 'NaN'$$) AS nan_ge,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 < 'Infinity'$$) AS pinf_lt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 'Infinity'$$) AS pinf_gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 >= 'Infinity'$$) AS pinf_ge,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <> 'Infinity'$$) AS pinf_ne,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > '-Infinity'$$) AS ninf_gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 < '-Infinity'$$) AS ninf_lt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 = '-Infinity'$$) AS ninf_eq;
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n38 IN (0, 1234.56, 9999999999999999999999999999.9999999999, -1)$$) AS in_list,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 IN (0, 'NaN')$$) AS in_nan,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 IN ('NaN', 'Infinity', 1e60, 0.00000000001)$$) AS in_unreachable,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 NOT IN (0, 1234.56, -1)$$) AS not_in,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 NOT IN (0, 'NaN')$$) AS not_in_nan,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 NOT IN ('Infinity', 1e60)$$) AS not_in_unreachable,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 IN (0, NULL)$$) AS in_null,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 NOT IN (0, NULL)$$) AS not_in_null,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 = ANY (ARRAY[]::numeric[])$$) AS in_empty,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 <> ALL (ARRAY[]::numeric[])$$) AS not_in_empty,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 IS NULL$$) AS is_null,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 IS NOT NULL$$) AS is_not_null,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 100 AND n38 < 200 AND n38 <> 150$$) AS three;
-- numeric(19, 2)
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n19 > -5000.5 AND n19 <= 99999999999999999.99$$) AS range19,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n19 = 99999999999999999.99$$) AS max19,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n19 < -99999999999999999.99$$) AS below_min19,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n19 = 12.345$$) AS finer19,
       same($$SELECT count(*), sum(qty) FROM wa WHERE n19 IS NULL$$) AS null19;

-- numeric of any shape: the image orders as the number does, 1.0 = 1.00, NaN is the greatest and equal to itself
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE img = 1$$) AS eq1,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img = 1.0000$$) AS eq1_scale,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img = 1.5$$) AS eq15,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img <> 1$$) AS ne1,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img > 1$$) AS gt1,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img >= 1.5$$) AS ge15,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img < -0.0001$$) AS lt_small,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img <= 0$$) AS le0,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img = 'NaN'$$) AS nan_eq,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img > 'Infinity'$$) AS above_inf,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img >= 'Infinity'$$) AS ge_inf,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img < '-Infinity'$$) AS below_ninf,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img <= '-Infinity'$$) AS le_ninf,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img IN (1, 1.5, 'NaN', 'Infinity', 0.0)$$) AS in_list,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img NOT IN (1, 1.5, 'NaN')$$) AS not_in,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img IS NULL$$) AS is_null,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img > -1000 AND img < 1000$$) AS between_,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img > 12345678901234567890.123456789$$) AS twenty_digits;
SELECT same(format($$SELECT count(*), sum(qty) FROM wa WHERE img > 1%s$$, repeat('0', 399))) AS huge_gt,
       same(format($$SELECT count(*), sum(qty) FROM wa WHERE img < -1%s$$, repeat('0', 399))) AS huge_lt,
       same(format($$SELECT count(*), sum(qty) FROM wa WHERE img > 0.%s1$$, repeat('0', 299))) AS tiny_gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE neg > -5000$$) AS negative_scale_gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE neg = 100$$) AS negative_scale_eq,
       same($$SELECT count(*), sum(qty) FROM wa WHERE neg = 150$$) AS negative_scale_off_unit,
       same($$SELECT count(*), sum(qty) FROM wa WHERE neg IN (100, -100, 12345)$$) AS negative_scale_in;

-- money: the integer of the smallest unit
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE m > '123.45'$$) AS gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m <= '-123.45'$$) AS le,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m = '92233720368547758.07'$$) AS max_,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m = '-92233720368547758.08'$$) AS min_,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m IN ('0', '1.00', '-1.00')$$) AS in_list,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m NOT IN ('0', '1.00')$$) AS not_in,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m IS NULL$$) AS is_null,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m <> '0' AND m > '-5.00' AND m < '5.00'$$) AS around_zero;

-- interval: the total orders and equates (1 month = 30 days = 720 hours), the infinities are the extremes
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE iv = interval '1 month'$$) AS eq_month,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv = interval '30 days'$$) AS eq_days,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv = interval '720 hours'$$) AS eq_hours,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv = interval '1 mon 1 day'$$) AS eq_31,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv > interval '1 month'$$) AS gt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv >= interval '1 month'$$) AS ge,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv < interval '-1 month'$$) AS lt,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv <= interval '0'$$) AS le0,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv <> interval '0'$$) AS ne0,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv > interval '59 days' AND iv <= interval '2 mons'$$) AS between_,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv IN (interval '1 month', interval '1 year', interval '-00:00:00.000001')$$) AS in_list,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv NOT IN (interval '0', interval '1 day')$$) AS not_in,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv = 'infinity'$$) AS eq_inf,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv < '-infinity'$$) AS lt_ninf,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv > interval '2147483647 months 2147483647 days 9223372036854775806 us'$$) AS near_max,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv IS NULL$$) AS is_null;

-- with other conjuncts, grouped, through the pre-aggregates (which a wide conjunct turns off), over a window of the table
SELECT same($$SELECT grp, count(*), sum(qty), min(qty), max(id) FROM wa WHERE n38 > 100 AND m < '500.00' AND iv >= interval '1 day' GROUP BY grp$$) AS grouped,
       same($$SELECT grp, count(*), sum(qty) FROM wa WHERE id BETWEEN 5000 AND 7000 AND img > 0 GROUP BY grp$$) AS windowed,
       same($$SELECT count(*), sum(qty) FROM wa WHERE id < 40 AND n38 IS NOT NULL$$) AS sparse,
       same($$SELECT count(*) FROM wa WHERE n38 > 1e60$$) AS none_pass,
       same($$SELECT count(*) FROM wa WHERE n38 IS NOT NULL OR true$$) AS gate_or;

-- parameters: a prepared statement is planned with the value unknown (a generic plan) and evaluated when the scan starts
PREPARE p_n(numeric) AS SELECT count(*), sum(qty) FROM wa WHERE n38 > $1;
PREPARE p_m(money) AS SELECT count(*), sum(qty) FROM wa WHERE m <= $1;
PREPARE p_iv(interval) AS SELECT count(*), sum(qty) FROM wa WHERE iv = $1;
PREPARE p_in(numeric[]) AS SELECT count(*), sum(qty) FROM wa WHERE img = ANY ($1);
SET plan_cache_mode = force_generic_plan;
SELECT same($$EXECUTE p_n(1234.5)$$) AS generic_numeric, same($$EXECUTE p_n('NaN')$$) AS generic_nan, same($$EXECUTE p_n(NULL)$$) AS generic_null_operand,
       same($$EXECUTE p_m('123.45')$$) AS generic_money, same($$EXECUTE p_iv('30 days')$$) AS generic_interval,
       same($$EXECUTE p_in(ARRAY[1, 1.5, 'NaN']::numeric[])$$) AS generic_array, same($$EXECUTE p_in(NULL)$$) AS generic_null_array,
       same($$EXECUTE p_in(ARRAY[NULL, 1]::numeric[])$$) AS generic_array_null_element;
RESET plan_cache_mode;
DEALLOCATE ALL;

-- stable functions are evaluated when the scan starts
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE iv <= now() - (now() - interval '30 days')$$) AS stable_operand,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m <= (SELECT max(m) FROM wa WHERE id < 100)$$) AS subquery_operand;

-- the write buffer, a deleted row, an updated one and a merged index give the same rows
INSERT INTO wa SELECT 30000 + i, i % 7, i % 100, CASE WHEN i % 7 = 0 THEN 'NaN' ELSE i * 1234567.1234567891 END, i / 3.0,
	   CASE WHEN i % 5 = 0 THEN 'NaN' ELSE round(i / 7.0, i % 9) END, 100, (i * 17 || '.' || i % 100)::money, make_interval(months => i % 14, days => i % 40) FROM generate_series(1, 3000) i;
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 1234.5$$) AS buffered_numeric,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img >= 1.5$$) AS buffered_image,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m > '123.45'$$) AS buffered_money,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv > interval '1 month'$$) AS buffered_interval;
DELETE FROM wa WHERE id % 5 = 1;
UPDATE wa SET n38 = round(n38 / 2, 10), m = m / 2, iv = iv + interval '1 day'
WHERE id % 11 = 2 AND id % 71 <> 0 AND id % 73 <> 0 AND id % 79 <> 0 AND id % 83 <> 0 AND id % 101 <> 0 AND id % 103 <> 0;
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 1234.5$$) AS updated_numeric,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m > '123.45'$$) AS updated_money,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv > interval '1 month'$$) AS updated_interval;
SELECT sift.col_promote('wa_col');
VACUUM wa;
SELECT sift.col_merge('wa_col', true);
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 1234.5$$) AS merged_numeric,
       same($$SELECT count(*), sum(qty) FROM wa WHERE img >= 1.5$$) AS merged_image,
       same($$SELECT count(*), sum(qty) FROM wa WHERE m > '123.45'$$) AS merged_money,
       same($$SELECT count(*), sum(qty) FROM wa WHERE iv > interval '1 month' AND n19 > 0$$) AS merged_interval;

-- parallel workers
RESET seq_page_cost;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET seq_page_cost = 1000;
SELECT same($$SELECT count(*), sum(qty) FROM wa WHERE n38 > 1234.5 AND m < '900.00'$$) AS parallel_numeric,
       same($$SELECT grp, count(*) FROM wa WHERE iv >= interval '1 month' GROUP BY grp$$) AS parallel_interval;

DROP TABLE wa;
DROP FUNCTION same(text), wide_in_source(text), scanned(text);
DROP EXTENSION pg_sift;
