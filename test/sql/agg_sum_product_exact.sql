-- The sum of a product of two decimal expressions whose digits together pass 38 (a price times one minus a discount times one plus a tax of three
-- decimal(15,2) is 47 digits; the digits proven are at most 48) multiplies each pair in 256 bits and keeps the sum in 128 bits plus the number of times it wrapped: values at the limit of the
-- digits proven, mixed signs, a NaN, NULLs, the average, a filter, a grouped and a parallel scan are exact. Every statement runs with sift.enable_agg = off
-- as well: the rows must be the same.
CREATE EXTENSION pg_sift;
CREATE TABLE p (id int PRIMARY KEY, g int NOT NULL, a numeric(16, 0), b numeric(16, 0), c numeric(16, 0), price numeric(15, 2), disc numeric(15, 2), tax numeric(15, 2));
INSERT INTO p
SELECT i, i % 4, 9999999999999999 - (i % 7), 9999999999999999 - (i % 5), CASE WHEN i % 11 = 0 THEN -9999999999999999 + i % 13 ELSE 9999999999999999 - i % 17 END,
       round((100 + (i * 7919 % 99000000))::numeric / 7, 2), (i % 11)::numeric / 100, (i % 9)::numeric / 100
FROM generate_series(1, 5000) i;
INSERT INTO p VALUES (10001, 9, 9999999999999999, 9999999999999999, 9999999999999999, 9999999999999.99, 0, 9999999999999.99),
                     (10002, 9, 9999999999999999, 9999999999999999, -9999999999999999, 9999999999999.99, 0, 9999999999999.99),
                     (10003, 8, 9999999999999999, 9999999999999999, 9999999999999999, 9999999999999.99, -9999999999999.99, 9999999999999.99),
                     (10004, 7, 9999999999999999, 9999999999999999, -9999999999999999, -9999999999999.99, 0.5, 0.5),
                     (10005, 6, NULL, 3, 4, NULL, 0.1, 0.2), (10006, 6, 5, NULL, 4, 1, NULL, 0.2), (10007, 6, 5, 6, 4, 1, 0.1, 0.2),
                     (10008, 5, 'NaN', 2, 3, 'NaN', 0.1, 0.2), (10009, 5, 1, 2, 3, 1, 0.1, 0.2);
INSERT INTO p SELECT 20000 + i, 4, 1234567890123456, 8765432109876543, 12345 + i, 1234567890123.45, 0.01 * (i % 90), 0.01 * (i % 70) FROM generate_series(1, 3000) i;
CREATE INDEX p_col ON p USING sift_col (g, a, b, c, price, disc, tax);
VACUUM (FREEZE, ANALYZE) p;

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
CREATE FUNCTION scanned(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	SET LOCAL sift.agg_force = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SET max_parallel_workers_per_gather = 0;

SELECT scanned('SELECT g, sum(a * b * c) FROM p GROUP BY g') AS wide_scanned, scanned('SELECT sum(price * (1 - disc) * (1 + tax)) FROM p') AS charge_scanned,
       scanned('SELECT sum(price * (1 - disc) * (1 + tax)), sum(DISTINCT price * (1 - disc) * (1 + tax)) FROM p') AS distinct_scanned;
SELECT g, sum(a * b * c) AS s FROM p WHERE g IN (7, 8, 9) GROUP BY g ORDER BY g;
SELECT differences('SELECT g, sum(a * b * c) AS s, avg(a * b * c) AS m, count(*) AS n FROM p GROUP BY g') AS grouped;
SELECT differences('SELECT sum(a * b * c) AS s, avg(a * b * c) AS m FROM p') AS ungrouped;
SELECT differences('SELECT g, sum(a * b * c) FILTER (WHERE c < 0) AS neg, sum(a * b * c) FILTER (WHERE c > 0) AS pos FROM p GROUP BY g') AS filtered;
SELECT differences('SELECT g, sum(price * (1 - disc) * (1 + tax)) AS charge, avg(price * (1 - disc) * (1 + tax)) AS m FROM p GROUP BY g') AS charge;
SELECT differences('SELECT g, sum(price * (1 - disc) * (1 + tax)) AS s1, sum(price * (1 - disc)) AS s2, sum(a * b) AS s3, count(c) AS n FROM p GROUP BY g') AS mixed;
SELECT differences('SELECT sum(price * (1 - disc) * (1 + tax)) AS s1, sum(a * b * c) AS s2 FROM p WHERE g = 6') AS nulls;
SELECT differences('SELECT sum(price * (1 - disc) * (1 + tax)) FROM p WHERE g = 5') AS not_a_number;
SELECT differences('SELECT g, sum(price * (1 - disc) * (1 + tax)) AS charge FROM p GROUP BY g ORDER BY charge DESC LIMIT 3') AS top;

-- sums that end at 2^127 - 1 (the integer of a sum is a number, not the NaN of the numeric programs): 48-digit products of two numeric(24, 0), products of two
-- numeric(19, 0), and pairs of 38-digit values that pass it
CREATE TABLE tm (id int PRIMARY KEY, g int NOT NULL, a numeric(24, 0), b numeric(24, 0), c numeric(19, 0), d numeric(19, 0));
WITH t AS (SELECT power(2::numeric, 127) - 1 AS top, 999999999999999999999999::numeric AS p, 9999999999999999999::numeric AS q)
INSERT INTO tm (id, g, a, b) SELECT 1, 1, floor(top / p), p FROM t UNION ALL SELECT 2, 1, top - floor(top / p) * p, 1 FROM t
	UNION ALL SELECT 3, 2, -floor(top / p), p FROM t UNION ALL SELECT 4, 2, -(top - floor(top / p) * p), 1 FROM t
	UNION ALL SELECT 5, 3, floor(top / p), p FROM t UNION ALL SELECT 6, 3, top - floor(top / p) * p + 1, 1 FROM t
	UNION ALL SELECT 7, 4, 999999999999999999999999, 99999999999999 UNION ALL SELECT 8, 4, 999999999999999999999999, 99999999999999;
WITH t AS (SELECT power(2::numeric, 127) - 1 AS top, 9999999999999999999::numeric AS q)
INSERT INTO tm (id, g, c, d) SELECT 11, 11, q, q FROM t UNION ALL SELECT 12, 11, floor((top - q * q) / q), q FROM t
	UNION ALL SELECT 13, 11, top - q * q - floor((top - q * q) / q) * q, 1 FROM t UNION ALL SELECT 14, 12, 3, 2;
CREATE INDEX tm_col ON tm USING sift_col (g, a, b, c, d);
VACUUM (FREEZE, ANALYZE) tm;
SELECT scanned('SELECT g, sum(a * b) FROM tm GROUP BY g') AS wide_pair_scanned, scanned('SELECT g, sum(c * d) FROM tm GROUP BY g') AS narrow_pair_scanned;
SELECT g, sum(a * b) AS s, avg(a * b) AS m FROM tm WHERE g IN (1, 2, 3) GROUP BY g ORDER BY g;
SELECT g, sum(c * d) AS s FROM tm WHERE g = 11 GROUP BY g;
SELECT differences('SELECT g, sum(a * b) AS s, avg(a * b) AS m, count(a * b) AS n FROM tm WHERE a IS NOT NULL GROUP BY g') AS pair_24;
SELECT differences('SELECT g, sum(c * d) AS s, avg(c * d) AS m, count(c * d) AS n FROM tm WHERE c IS NOT NULL GROUP BY g') AS pair_19;

-- random values: signs, NULLs, 1 to the declared digits (a tenth of them all 9s), scales 0 to 16, groups that are empty or all NULL; a product of 48 digits is
-- and a product of 49 or more goes through numeric_mul for each row, as before (the 49th digit must not overflow the 256-bit product)
SELECT setseed(0.31);
CREATE FUNCTION rnum(p int, s int) RETURNS numeric LANGUAGE sql AS $$
	SELECT CASE WHEN r < 0.05 THEN NULL
	            ELSE (CASE WHEN r < 0.15 THEN repeat('9', p) ELSE (SELECT string_agg(floor(random() * 10)::int::text, '') FROM generate_series(1, 1 + floor(random() * p)::int)) END)::numeric
	                 * (10::numeric ^ -s) * (CASE WHEN random() < 0.4 THEN -1 ELSE 1 END) END
	FROM (SELECT random() AS r) q $$;
CREATE TABLE rnd (id int PRIMARY KEY, g int NOT NULL, x numeric(16, 0), y numeric(16, 5), z numeric(16, 10), u numeric(17, 0), a numeric(24, 0), b numeric(24, 12), c numeric(16, 8));
INSERT INTO rnd SELECT i, i % 37, rnum(16, 0), rnum(16, 5), rnum(16, 10), rnum(17, 0), rnum(24, 0), rnum(24, 12), rnum(16, 8) FROM generate_series(1, 8000) i;
INSERT INTO rnd SELECT 10000 + i, 99, NULL, NULL, NULL, NULL, NULL, NULL, NULL FROM generate_series(1, 20) i;
CREATE INDEX rnd_col ON rnd USING sift_col (g, x, y, z, u, a, b, c);
VACUUM (FREEZE, ANALYZE) rnd;
SELECT count(*) = 8020 AS rows, count(x * y * z) > 6000 AS d48_rows, count(u * y * z) > 6000 AS d49_rows, count(a * b * c) > 6000 AS d64_rows, min(u * y * z) < -1e30 AS d49_wide FROM rnd;
SELECT differences('SELECT g, sum(x * y * z) AS s, avg(x * y * z) AS m, count(x * y * z) AS n FROM rnd GROUP BY g') AS d48_grouped;
SELECT differences('SELECT sum(x * y * z) AS s, avg(x * y * z) AS m FROM rnd') AS d48_ungrouped;
SELECT differences('SELECT g, sum(a * b) AS s, avg(a * b) AS m FROM rnd GROUP BY g') AS wide48_grouped;
SELECT differences('SELECT sum(a * b) FILTER (WHERE a < 0) AS neg, sum(x * y * z) FILTER (WHERE x > 0 AND z < 0) AS mixed FROM rnd') AS filtered;
SELECT differences('SELECT g, sum(x * y * z) AS s, avg(x * y * z) AS m FROM rnd WHERE g > 98 GROUP BY g') AS all_null_group;
SELECT differences('SELECT sum(x * y * z) AS s, avg(x * y * z) AS m FROM rnd WHERE g > 1000') AS empty;
SELECT differences('SELECT g, sum(u * y * z) AS s FROM rnd GROUP BY g') AS d49_grouped;
SELECT differences('SELECT g, sum(a * b * c) AS s FROM rnd GROUP BY g') AS d64_grouped;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT differences('SELECT g, sum(a * b * c) AS s, avg(price * (1 - disc) * (1 + tax)) FROM p GROUP BY g') AS parallel_grouped;
SELECT differences('SELECT sum(a * b * c) AS s, avg(a * b * c) FROM p') AS parallel_ungrouped;
SELECT differences('SELECT g, sum(x * y * z) AS s, avg(a * b) AS m FROM rnd GROUP BY g') AS parallel_random;

DROP FUNCTION differences(text), scanned(text);
DROP FUNCTION rnum(int, int);
DROP TABLE p, rnd, tm;
DROP EXTENSION pg_sift;
