-- SiftAggV6 over the expressions of the TPC-H queries, over a sift_col index and a sift_table, as the heap answers them: the exact scaled-integer product of up to three affine factors of numeric
-- columns (a * (1 - b) * (1 + c), with NULLs, negative values and values wide enough that a product leaves int64 and 128 bits), a column against a column (l_commitdate < l_receiptdate), OR of quals, a numeric
-- against a constant of another scale, and CASE WHEN .. THEN .. ELSE 0 END as FILTER (ungrouped and grouped, over groups with no row that passes and over no rows). Every check prints t; plan_ok prints t
-- when the statement ran as SiftAggV6.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET cpu_tuple_cost = 0.5;

CREATE TABLE th (id int PRIMARY KEY, grp int, a numeric(12,2), b numeric(12,2), c numeric(12,2), w numeric(18,4), d1 date, d2 date, n4 int, i8 bigint, f8 float8, tag text) WITH (autovacuum_enabled = false);
CREATE TABLE tc (LIKE th INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE tt (LIKE th INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO th
SELECT g, g % 7,
	CASE WHEN g % 11 = 0 THEN NULL ELSE ((g * 37) % 100000 - 20000) / 100.0 END,
	CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 100) / 100.0 END,
	CASE WHEN g % 17 = 0 THEN NULL ELSE (g % 8) / 100.0 END,
	CASE WHEN g % 19 = 0 THEN NULL ELSE (90000000000000 + g * 1234567::bigint)::numeric + 0.1234 END,
	CASE WHEN g % 23 = 0 THEN NULL ELSE date '2020-01-01' + g % 400 END,
	CASE WHEN g % 29 = 0 THEN NULL ELSE date '2020-01-01' + g % 400 + (g * 7) % 20 - 5 END,
	CASE WHEN g % 31 = 0 THEN NULL ELSE (g * 13) % 50 END,
	g * 1000000007::bigint,
	g / 7.0,
	(ARRAY['R', 'A', 'N', 'O'])[1 + g % 4]
FROM generate_series(1, 6000) g;
INSERT INTO tc SELECT * FROM th;
INSERT INTO tt SELECT * FROM th;
CREATE INDEX tc_sift ON tc USING sift_col (id, grp, a, b, c, w, d1, d2, n4, i8, f8, tag);
SELECT sift.table_maintain('tt') IS NOT NULL AS maintained;
ANALYZE th, tc, tt;

CREATE FUNCTION same_rows(a text, b text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', a) INTO x;
	EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', b) INTO y;
	RETURN x = y;
END $$;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT same_rows(replace(q, '@T', 'tc'), replace(q, '@T', 'th')) AND same_rows(replace(q, '@T', 'tt'), replace(q, '@T', 'th')) $$;

CREATE FUNCTION plan_ok(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'tc') LOOP n := n + (line LIKE '%SiftAggV6%')::int; END LOOP;
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', 'tt') LOOP n := n + (line LIKE '%SiftAggV6%')::int; END LOOP;
	RETURN n = 2;
END $$;

CREATE FUNCTION plan_index(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP n := n + (line LIKE '%SiftAggV6%')::int; END LOOP;
	RETURN n = 1;
END $$;

-- the scale of the result is the scale numeric_mul gives, not only its value
CREATE FUNCTION parity_text(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE replace(q, '@T', 'tc') INTO x;
	EXECUTE replace(q, '@T', 'th') INTO y;
	RETURN x = y;
END $$;

-- ---- the exact numeric polynomial ----
SELECT plan_ok('SELECT sum(a * (1 - b)) FROM @T') AS product_plan;
SELECT parity('SELECT sum(a * (1 - b)), sum(a * (1 - b) * (1 + c)), avg(a * (1 - b)), sum(a * b), sum(1 + a), sum(a - 2.5), sum((a + 1.5) * (b - 0.5)) FROM @T') AS products_ungrouped;
SELECT parity('SELECT grp, sum(a * (1 - b)), sum(a * (1 - b) * (1 + c)), avg(a * (1 - b) * (1 + c)), count(a * b) FROM @T GROUP BY grp') AS products_grouped;
SELECT parity('SELECT tag, sum(a * (1 - b)), sum(a * (1 - b) * (1 + c)), sum(a) FROM @T WHERE d1 >= date ''2020-03-01'' GROUP BY tag') AS products_filtered;
SELECT parity('SELECT sum(a * (1 - b)) FILTER (WHERE grp = 3), sum(a * (1 - b) * (1 + c)) FILTER (WHERE tag = ''R''), count(*) FILTER (WHERE a IS NULL) FROM @T') AS products_under_filter;
SELECT parity('SELECT grp, sum(a * (1 - b)) FILTER (WHERE tag = ''R''), sum(a * (1 - b) * (1 + c)) FILTER (WHERE tag = ''A'') FROM @T GROUP BY grp') AS products_under_filter_grouped;
SELECT parity_text('SELECT sum(a * (1 - b))::text FROM @T') AS result_scale_of_a_product;
SELECT parity_text('SELECT sum(a * (1 - b) * (1 + c))::text FROM @T') AS result_scale_of_three_factors;
SELECT parity_text('SELECT (sum(a * (1 - b)) / sum(a))::text FROM @T') AS quotient_of_products;
SELECT parity('SELECT sum(a * (1 - b)) FROM @T WHERE a IS NULL') AS only_null_a;
SELECT parity('SELECT sum(a * (1 - b)), avg(a * (1 - b)), count(a * (1 - b)) FROM @T WHERE id < 0') AS no_rows;
-- values wide enough that a product leaves int64 (and a three-factor product stays in 128 bits)
SELECT plan_ok('SELECT sum(w * (1 - b)) FROM @T') AS wide_plan;
SELECT parity('SELECT sum(w * (1 - b)), sum(w * (1 - b) * (1 + c)), avg(w * (1 - b)), sum(w * 3) FROM @T') AS wide_products;
SELECT parity('SELECT grp, sum(w * (1 - b)), sum(w * (1 - b) * (1 + c)) FROM @T GROUP BY grp') AS wide_grouped;
SELECT parity('SELECT sum(w * (1 - b)) FILTER (WHERE tag <> ''N'') FROM @T') AS wide_under_filter;
SELECT parity('SELECT sum(a * (1 - b) * (1 + c)), sum(a * (1 - b)) FROM @T WHERE id <= 40') AS small_after_wide_free;
-- a product past 2^95 scaled is answered (sums of 192 bits); one of 2^191 or more is refused, never answered wrongly
CREATE FUNCTION refused(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text;
BEGIN
	EXECUTE q INTO x;
	RETURN false;
EXCEPTION WHEN numeric_value_out_of_range THEN RETURN true;
END $$;
SELECT refused('SELECT sum(w * w * w)::text FROM tc WHERE w IS NOT NULL') AS beyond_128_bits_refused;
SELECT parity('SELECT sum(w * w)::text, avg(w * w)::text FROM @T WHERE w IS NOT NULL') AS beyond_2_95_answered;

-- ---- a column against a column, OR, numerics against constants ----
SELECT plan_ok('SELECT count(*) FROM @T WHERE d1 < d2') AS column_compare_plan;
SELECT parity('SELECT count(*), sum(a) FROM @T WHERE d1 < d2') AS dates_less;
SELECT parity('SELECT grp, count(*) FROM @T WHERE d1 = d2 GROUP BY grp') AS dates_equal;
SELECT parity('SELECT grp, count(*) FROM @T WHERE d1 <> d2 AND d2 >= date ''2020-03-01'' GROUP BY grp') AS dates_not_equal;
SELECT parity('SELECT count(*) FROM @T WHERE a < b') AS numerics_less;
SELECT parity('SELECT count(*) FROM @T WHERE a >= b * 100') AS numeric_expression_compare;
SELECT parity('SELECT count(*) FROM @T WHERE n4 > grp') AS ints_greater;
SELECT parity('SELECT count(*) FROM @T WHERE i8 > n4') AS int8_against_int4;
SELECT parity('SELECT count(*) FROM @T WHERE n4 <= grp OR d1 > d2') AS or_of_compares;
SELECT plan_ok('SELECT sum(a) FROM @T WHERE (n4 > 5 AND grp = 1) OR (d1 < d2 AND n4 = 2)') AS or_plan;
SELECT plan_index('SELECT sum(a) FROM tc WHERE (a > 5 AND grp = 1) OR (b < 0.03 AND n4 = 2)') AS or_numeric_plan_index;
SELECT parity('SELECT sum(a), count(*) FROM @T WHERE (a > 5 AND grp = 1) OR (b < 0.03 AND n4 = 2)') AS or_of_ands;
SELECT parity('SELECT grp, sum(a), count(*) FROM @T WHERE (a > 5 AND tag = ''R'') OR (b < 0.03 AND tag = ''A'') OR n4 IS NULL GROUP BY grp') AS or_three_branches;
SELECT parity('SELECT count(*) FROM @T WHERE NOT (a > 5 OR b < 0.5)') AS not_of_or;
SELECT parity('SELECT count(*) FROM @T WHERE a > 5 OR a IS NULL') AS or_null_test;
SELECT parity('SELECT count(*) FROM @T WHERE b < 0.055') AS numeric_constant_of_a_longer_scale;
SELECT parity('SELECT count(*) FROM @T WHERE b = 0.5') AS numeric_constant_of_a_shorter_scale;
SELECT parity('SELECT count(*) FROM @T WHERE b BETWEEN 0.05 AND 0.07 AND a < 24') AS numeric_between_and_integer_constant;
SELECT parity('SELECT sum(a * (1 - b)) FROM @T WHERE b BETWEEN 0.05 AND 0.07 AND a < 24 AND d1 >= date ''2020-02-01'' AND d1 < date ''2020-09-01''') AS q6_shape;
SELECT parity('SELECT count(*) FROM @T WHERE a = ANY (ARRAY[1.5, 2.25, 100])') AS numeric_in_list;
SELECT parity('SELECT count(*) FROM @T WHERE f8 > n4') AS float8_against_int4_refused_or_exact;

-- ---- CASE WHEN .. THEN .. ELSE 0 END as FILTER ----
SELECT plan_ok('SELECT grp, sum(CASE WHEN n4 IN (1, 2) THEN 1 ELSE 0 END), sum(CASE WHEN n4 NOT IN (1, 2) THEN 1 ELSE 0 END) FROM @T GROUP BY grp') AS case_plan;
SELECT parity('SELECT grp, sum(CASE WHEN n4 IN (1, 2) THEN 1 ELSE 0 END), sum(CASE WHEN n4 NOT IN (1, 2) THEN 1 ELSE 0 END), sum(CASE WHEN n4 > 40 THEN a ELSE 0 END), sum(CASE WHEN n4 <> 7 THEN a END) FROM @T GROUP BY grp') AS case_int_conditions;
SELECT parity('SELECT grp, sum(CASE WHEN tag IN (''R'', ''A'') THEN 1 ELSE 0 END), sum(CASE WHEN tag NOT IN (''R'', ''A'') THEN 1 ELSE 0 END) FROM @T GROUP BY grp') AS case_count_pivot;
SELECT parity('SELECT sum(CASE WHEN tag = ''R'' THEN 1 ELSE 0 END), sum(CASE WHEN tag <> ''R'' THEN 1 ELSE 0 END), sum(CASE WHEN tag = ''R'' THEN 1 END), count(CASE WHEN tag = ''R'' THEN 1 END) FROM @T') AS case_count_ungrouped;
SELECT parity('SELECT sum(CASE WHEN tag = ''Z'' THEN 1 ELSE 0 END), sum(CASE WHEN tag = ''Z'' THEN a ELSE 0 END), sum(CASE WHEN tag = ''Z'' THEN a END) FROM @T') AS no_row_passes;
SELECT parity('SELECT grp, sum(CASE WHEN tag = ''Z'' THEN a ELSE 0 END), sum(CASE WHEN tag = ''R'' THEN a ELSE 0 END), sum(CASE WHEN tag = ''R'' THEN a END) FROM @T GROUP BY grp') AS no_row_passes_grouped;
SELECT parity('SELECT sum(CASE WHEN tag = ''R'' THEN 1 ELSE 0 END), sum(CASE WHEN tag = ''R'' THEN a ELSE 0 END), count(CASE WHEN tag = ''R'' THEN a END) FROM @T WHERE id < 0') AS case_over_no_rows;
SELECT parity('SELECT sum(CASE WHEN a IS NULL THEN b ELSE 0 END), sum(CASE WHEN tag = ''R'' THEN a ELSE 0 END), sum(CASE WHEN tag = ''R'' THEN a * (1 - b) ELSE 0 END) FROM @T') AS case_over_nulls_and_products;
SELECT parity('SELECT grp, sum(CASE WHEN tag = ''R'' THEN a * (1 - b) ELSE 0 END), sum(a * (1 - b)) FROM @T GROUP BY grp') AS case_of_a_product_grouped;
SELECT parity_text('SELECT (100.00 * sum(CASE WHEN tag IN (''R'', ''A'') THEN a * (1 - b) ELSE 0 END) / sum(a * (1 - b)))::text FROM @T WHERE d1 >= date ''2020-02-01'' AND d1 < date ''2020-03-01''') AS promo_revenue_shape;
SELECT parity('SELECT sum(CASE WHEN tag = ''R'' THEN a ELSE 0 END) FROM @T WHERE tag = ''N''') AS case_never_true_with_rows;
SELECT parity('SELECT grp, sum(CASE WHEN tag = ''R'' THEN 1 ELSE 0 END) FROM @T GROUP BY grp HAVING sum(CASE WHEN tag = ''R'' THEN 1 ELSE 0 END) > 100') AS case_in_having;
SELECT parity('SELECT grp, sum(CASE WHEN tag = ''R'' THEN 2 ELSE 0 END), round(sum(CASE WHEN tag = ''R'' THEN f8 ELSE 0 END)::numeric, 6) FROM @T GROUP BY grp') AS case_not_rewritten_still_right;
SELECT parity('SELECT sum(CASE WHEN d1 < d2 THEN 1 ELSE 0 END) FROM @T') AS case_with_column_compare_is_left_to_the_heap;

-- ---- the whole of the queries: the shapes of TPC-H 1, 4, 6, 12, 14, 19 over one table ----
SELECT plan_ok('SELECT grp, n4, sum(a), sum(a * (1 - b)), sum(a * (1 - b) * (1 + c)), avg(b), count(*) FROM @T WHERE d1 <= date ''2020-12-01'' GROUP BY grp, n4') AS q1_plan;
SELECT parity('SELECT tag, grp, sum(a), sum(a * (1 - b)), sum(a * (1 - b) * (1 + c)), avg(a), avg(b), count(*) FROM @T WHERE d1 <= date ''2020-12-01'' GROUP BY tag, grp') AS q1;
SELECT plan_ok('SELECT grp, count(*) FROM @T WHERE d1 >= date ''2020-03-01'' AND d1 < date ''2020-06-01'' AND d1 < d2 GROUP BY grp') AS q4_plan;
SELECT parity('SELECT tag, count(*) FROM @T WHERE d1 >= date ''2020-03-01'' AND d1 < date ''2020-06-01'' AND d1 < d2 GROUP BY tag') AS q4;
SELECT plan_ok('SELECT n4, sum(CASE WHEN grp IN (1, 2) THEN 1 ELSE 0 END), sum(CASE WHEN grp NOT IN (1, 2) THEN 1 ELSE 0 END) FROM @T WHERE d1 < d2 AND d2 >= date ''2020-03-01'' GROUP BY n4') AS q12_plan;
SELECT parity('SELECT tag, sum(CASE WHEN grp IN (1, 2) THEN 1 ELSE 0 END), sum(CASE WHEN grp NOT IN (1, 2) THEN 1 ELSE 0 END) FROM @T WHERE d1 < d2 AND d2 >= date ''2020-03-01'' GROUP BY tag') AS q12;
SELECT plan_ok('SELECT sum(a * (1 - b)) FROM @T WHERE (grp = 1 AND n4 < 100 AND d1 < d2) OR (grp = 2 AND n4 >= 100 AND d1 < d2)') AS q19_plan;
SELECT plan_index('SELECT sum(a * (1 - b)) FROM tc WHERE (grp = 1 AND a < 100 AND b > 0.5) OR (grp = 2 AND a >= 100 AND c < 0.04)') AS q19_numeric_plan_index;
SELECT parity('SELECT sum(a * (1 - b)) FROM @T WHERE (tag = ''R'' AND a < 100 AND b > 0.5) OR (tag = ''A'' AND a >= 100 AND c < 0.04)') AS q19;

-- ---- DML keeps the answers ----
DELETE FROM th WHERE id % 5 = 0;
DELETE FROM tc WHERE id % 5 = 0;
DELETE FROM tt WHERE id % 5 = 0;
UPDATE th SET b = 0.99, a = 1234.56 WHERE id % 7 = 1;
UPDATE tc SET b = 0.99, a = 1234.56 WHERE id % 7 = 1;
UPDATE tt SET b = 0.99, a = 1234.56 WHERE id % 7 = 1;
INSERT INTO th SELECT g + 10000, g % 7, g / 4.0, 0.25, 0.01, 123.4567, date '2021-01-01', date '2021-01-02', g % 50, g, g, 'R' FROM generate_series(1, 300) g;
INSERT INTO tc SELECT * FROM th WHERE id > 10000;
INSERT INTO tt SELECT * FROM th WHERE id > 10000;
SELECT parity('SELECT tag, sum(a * (1 - b)), sum(a * (1 - b) * (1 + c)), count(*) FROM @T WHERE d1 < d2 GROUP BY tag') AS after_dml_products_and_compare;
SELECT parity('SELECT grp, sum(CASE WHEN tag = ''R'' THEN a ELSE 0 END), count(*) FROM @T WHERE (a > 5 AND grp = 1) OR n4 = 2 GROUP BY grp') AS after_dml_case_and_or;
BEGIN;
DELETE FROM tc WHERE id < 100;
DELETE FROM th WHERE id < 100;
SELECT parity_text('SELECT sum(a * (1 - b))::text FROM @T') AS inside_a_transaction;
ROLLBACK;
SELECT parity_text('SELECT sum(a * (1 - b))::text FROM @T') AS after_rollback;

-- ---- a NaN of a numeric: a product of it is NaN, a sum of that NaN ----
INSERT INTO th VALUES (900001, 1, 'NaN', 0.5, 0.1, 1, date '2000-01-01', date '2000-01-02', 1, 1, 1, 'R');
INSERT INTO tc SELECT * FROM th WHERE id = 900001;
INSERT INTO tt SELECT * FROM th WHERE id = 900001;
SELECT parity('SELECT sum(a * (1 - b))::text, avg(a * (1 - b))::text, count(a * (1 - b)) FROM @T') AS nan_product_answered;
SELECT same_rows('SELECT count(*) FROM tc WHERE a > 1', 'SELECT count(*) FROM th WHERE a > 1') AS nan_qual_answered_on_an_index;
SELECT same_rows('SELECT count(*) FROM tt WHERE a > 1', 'SELECT count(*) FROM th WHERE a > 1') AS nan_qual_answered_by_the_executor_on_a_table;

DROP FUNCTION same_rows(text, text), parity(text), plan_ok(text), plan_index(text), parity_text(text), refused(text);
DROP TABLE th, tc, tt;
DROP EXTENSION pg_sift;
