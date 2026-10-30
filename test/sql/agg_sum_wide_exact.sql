-- The sum of a decimal expression of up to 38 digits is kept in 128 bits plus the number of times it wrapped: a group whose sum passes 2^127 (1.7e38) and one whose
-- partial sums cancel out are exact, as are the average, a parallel scan and a grouped scan. Every statement runs with sift.enable_agg = off as well: the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
CREATE TABLE lines (id int PRIMARY KEY, g int NOT NULL, a numeric(18, 0) NOT NULL, b numeric(18, 0) NOT NULL, c numeric(2, 0) NOT NULL, price numeric(12, 2) NOT NULL, disc numeric(12, 2) NOT NULL, tax numeric(12, 2) NOT NULL);
INSERT INTO lines
SELECT i, i % 4, 999999999999999999 - (i % 7), 999999999999999999 - (i % 5), CASE WHEN i % 11 = 0 THEN -99 ELSE 99 END,
       round((100 + (i * 7919 % 99000000))::numeric / 7, 2), (i % 11)::numeric / 100, (i % 9)::numeric / 100
FROM generate_series(1, 5000) i;
-- a group of mixed signs that ends near zero, and a group of one huge value
INSERT INTO lines VALUES (10001, 9, 999999999999999999, 999999999999999999, 99, 1, 0, 0), (10002, 9, 999999999999999999, 999999999999999999, -99, 1, 0, 0),
                         (10003, 8, 999999999999999999, 999999999999999999, 99, 1, 0, 0);
CREATE INDEX lines_col ON lines USING sift_col (g, a, b, c, price, disc, tax);
VACUUM (FREEZE, ANALYZE) lines;

CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_force = on;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;
CREATE FUNCTION scanned(q text) RETURNS boolean LANGUAGE sql AS $$ SELECT sift_helpers.plan_mentions_siftagg_forced(q) $$;
SET max_parallel_workers_per_gather = 0;

SELECT scanned('SELECT g, sum(a * b * c) FROM lines GROUP BY g') AS wide_sum_scanned, scanned('SELECT sum(price * (1 - disc) * (1 + tax)) FROM lines') AS charge_scanned;
SELECT g, sum(a * b * c) AS s FROM lines WHERE g IN (8, 9) GROUP BY g ORDER BY g;
SELECT differences('SELECT g, sum(a * b * c) AS s, avg(a * b * c) AS m, count(*) AS n FROM lines GROUP BY g') AS grouped;
SELECT differences('SELECT sum(a * b * c) AS s, avg(a * b * c) AS m FROM lines') AS ungrouped;
SELECT differences('SELECT g, sum(a * b * c) FILTER (WHERE c < 0) AS neg, sum(a * b * c) FILTER (WHERE c > 0) AS pos FROM lines GROUP BY g') AS filtered;
SELECT differences('SELECT g, sum(price * (1 - disc) * (1 + tax)) AS charge, avg(price * (1 - disc) * (1 + tax)) AS m FROM lines GROUP BY g') AS charge;
SELECT differences('SELECT g, sum(price) AS p, sum(price * disc) AS pd, sum(a * 100000) AS big FROM lines GROUP BY g') AS plain;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT differences('SELECT g, sum(a * b * c) AS s FROM lines GROUP BY g') AS parallel_grouped;
SELECT differences('SELECT sum(a * b * c) AS s, avg(a * b * c) FROM lines') AS parallel_ungrouped;

DROP FUNCTION differences(text), scanned(text);
DROP TABLE lines;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
