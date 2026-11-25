-- SiftAggV6 answers what it is asked, as the heap does, over a sift_col index with an expression column, a range on a scaled numeric (NaN rows included) and a FILTER aggregate of a sift_table grouped
-- by a key. Every check prints t; plan_ok prints t when the statement ran as SiftAggV6.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET sift.test_create_format1 = off;
SET sift.agg_force = on;

CREATE TABLE ux_h (id int PRIMARY KEY, grp int, amount numeric(12,2), qty int, at timestamptz) WITH (autovacuum_enabled = false);
CREATE TABLE ux_c (LIKE ux_h INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE ux_t (LIKE ux_h INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO ux_h
SELECT g, g % 7, CASE WHEN g % 11 = 0 THEN 'NaN'::numeric WHEN g % 13 = 0 THEN NULL ELSE (g % 100) / 4.0 END, g % 5,
	timestamptz '2030-01-01' + g * interval '1 hour'
FROM generate_series(1, 4000) g;
INSERT INTO ux_c SELECT * FROM ux_h;
INSERT INTO ux_t SELECT * FROM ux_h;
CREATE INDEX ux_c_sift ON ux_c USING sift_col (id, grp, amount, qty, at, (qty + grp));
SELECT sift.table_maintain('ux_t') IS NOT NULL AS maintained;
ANALYZE ux_c;
ANALYZE ux_t;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT sift_helpers.same_rows(replace(q, '@T', 'ux_c'), replace(q, '@T', 'ux_h')) AND sift_helpers.same_rows(replace(q, '@T', 'ux_t'), replace(q, '@T', 'ux_h')) $$;

CREATE FUNCTION plan_ok(q text, t text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || replace(q, '@T', t) LOOP n := n + (line LIKE '%SiftAggV6%')::int; END LOOP;
	RETURN n = 1;
END $$;

SELECT plan_ok('SELECT grp, count(*), sum(amount) FROM @T WHERE amount > 10.00 AND amount <= 20.50 GROUP BY grp', 'ux_c') AS numeric_range_index_plan;
SELECT parity('SELECT grp, count(*), sum(amount) FROM @T WHERE amount > 10.00 AND amount <= 20.50 GROUP BY grp') AS numeric_range_parity;
SELECT parity('SELECT grp, count(*), min(amount), max(amount) FROM @T WHERE amount < 5.00 GROUP BY grp') AS numeric_range_below_parity;
SELECT parity('SELECT grp, count(*) FROM @T WHERE amount >= 24.75 GROUP BY grp') AS numeric_range_above_parity;

SELECT plan_ok('SELECT qty + grp AS k, count(*) FROM ux_c WHERE amount > 1.00 GROUP BY 1', 'ux_c') AS expression_index_plan;
SELECT sift_helpers.same_rows('SELECT qty + grp AS k, count(*), sum(amount) FROM ux_c WHERE amount > 1.00 GROUP BY 1', 'SELECT qty + grp AS k, count(*), sum(amount) FROM ux_h WHERE amount > 1.00 GROUP BY 1') AS expression_index_parity;

SELECT plan_ok('SELECT grp, count(*) FILTER (WHERE qty = 1) AS q1, sum(qty) FILTER (WHERE qty >= 2) AS q2 FROM @T GROUP BY grp', 'ux_t') AS filter_table_plan;
SELECT parity('SELECT grp, count(*) FILTER (WHERE qty = 1) AS q1, sum(qty) FILTER (WHERE qty >= 2) AS q2 FROM @T GROUP BY grp') AS filter_table_parity;

DROP TABLE ux_h, ux_c, ux_t;
DROP FUNCTION parity(text), plan_ok(text, text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
