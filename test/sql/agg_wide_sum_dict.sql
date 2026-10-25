-- sum and avg of a numeric without a precision (or of more than 18 digits) are added from the entries of the unit's dictionary: the integer and scale of an
-- entry are decoded once, a row adds them. Every statement is run with the scan forced and with sift.enable_agg = off and the results compared as text, so
-- that the display scale (the largest scale of the rows added) is compared too.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TABLE s (id serial, g int, k int, x numeric, w numeric(30,5)) WITH (autovacuum_enabled = false);
-- values of many scales, signs and sizes; NULLs; a group of NaN, one of Infinity; values past 38 digits; sums that overflow 128 bits
INSERT INTO s (g, k, x, w)
SELECT i % 5, i % 2, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919 % 100003)::numeric / (10 ^ (i % 6))::numeric END, ((i * 31 % 10007)::numeric - 5000) / 100
FROM generate_series(1, 6000) i;
INSERT INTO s (g, k, x) VALUES (5, 0, 1.10), (5, 0, 2.2000), (5, 0, 3), (5, 1, 0.5), (5, 1, -0.5), (5, 1, 0.00);
INSERT INTO s (g, k, x) VALUES (6, 0, 1), (6, 0, 'NaN'), (6, 1, 2.50), (7, 0, 1), (7, 0, 'Infinity'), (7, 1, 1), (7, 1, '-Infinity');
INSERT INTO s (g, k, x) VALUES (8, 0, 1e60), (8, 0, 2.5e60), (8, 1, -1e-60), (8, 1, 1e-60), (8, 1, 123456789012345678901234567890123456789012345.678);
INSERT INTO s (g, k, x) SELECT 9, 0, 99999999999999999999999999999999999999 FROM generate_series(1, 50);
INSERT INTO s (g, k, x) SELECT 10, 0, NULL FROM generate_series(1, 20);
INSERT INTO s (g, k, x) SELECT 11, i % 2, 0.001 * i FROM generate_series(1, 3000) i;
-- sums that end at 2^127 - 1 (the integer of a sum is a number, not the NaN of the programs) or pass it, and 38-digit values that add past 128 bits
INSERT INTO s (g, k, x) VALUES
 (20, 0, 170141183460469231731687303715884105727), (21, 0, 1), (21, 1, 170141183460469231731687303715884105726),
 (22, 0, 1.70141183460469231731687303715884105727), (23, 0, 170141183460469231731687303715884105727), (23, 1, 5),
 (24, 0, 99999999999999999999999999999999999999), (24, 1, 99999999999999999999999999999999999999),
 (25, 0, 99999999999999999999999999999999999999), (25, 1, 100000000000000000000000000000000000000), (25, 1, -100000000000000000000000000000000000000),
 (26, 0, -170141183460469231731687303715884105727), (27, 0, 170141183460469231731687303715884105725.5), (27, 1, 1.5),
 (28, 0, 5), (28, 1, 170141183460469231731687303715884105727);
CREATE INDEX s_col ON s USING sift_col (g, k, x, w);
VACUUM (FREEZE, ANALYZE) s;

CREATE FUNCTION answer(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	PERFORM set_config('sift.agg_force', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || q || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION used(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.agg_force', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE shapes (n int, q text);
INSERT INTO shapes VALUES
 (1, 'SELECT g, sum(x), avg(x), count(x) FROM s GROUP BY g'),
 (2, 'SELECT g, k, sum(x), avg(x) FROM s GROUP BY g, k'),
 (3, 'SELECT sum(x), avg(x), sum(w), avg(w) FROM s'),
 (4, 'SELECT g, sum(x) FILTER (WHERE k = 1), avg(x) FILTER (WHERE x > 10) FROM s GROUP BY g'),
 (5, 'SELECT g, sum(x), min(x), max(x) FROM s GROUP BY g'),
 (6, 'SELECT g, sum(x), count(DISTINCT x) FROM s GROUP BY g'),
 (7, 'SELECT g, sum(x) FROM s WHERE x > 5 GROUP BY g'),
 (8, 'SELECT g, sum(w), avg(w) FROM s GROUP BY g'),
 (9, 'SELECT sum(x) FROM s WHERE g = 10'),
 (10, 'SELECT g, sum(x), avg(x) FROM s WHERE id <= 3000 GROUP BY g'),
 (11, 'SELECT g, sum(x), avg(x), count(x) FROM s WHERE g >= 20 GROUP BY g');

SELECT n, answer(q, true) = answer(q, false) AS same_answer, used(q) AS scan, answer(q, true) <> '' AS not_empty FROM shapes ORDER BY n;
-- the figures
SET sift.agg_force = on;
SELECT g, sum(x), avg(x) FROM s WHERE g IN (20, 21, 22, 23) GROUP BY g ORDER BY g;
RESET sift.agg_force;

-- more than one unit, rows added and deleted since the build, a transaction's own changes
INSERT INTO s (g, k, x) SELECT 12, i % 3, i::numeric / 7 FROM generate_series(1, 4000) i;
DELETE FROM s WHERE id % 13 = 0;
SELECT n, answer(q, true) = answer(q, false) AS same_answer FROM shapes ORDER BY n;
BEGIN;
UPDATE s SET x = x * 3 WHERE id % 17 = 0;
INSERT INTO s (g, k, x) VALUES (1, 0, 123.456789), (1, 1, NULL);
SELECT n, answer(q, true) = answer(q, false) AS same_answer FROM shapes ORDER BY n;
ROLLBACK;

-- workers
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT n, answer(q, true) = answer(q, false) AS same_answer FROM shapes ORDER BY n;

-- random values: signs, NULLs, an occasional NaN, 1 to 60 digits (a tenth of them all 9s: sums that pass 128 bits and fall back to numeric_add), scales 0 to 30
-- (the display scale of a sum is the largest of the rows added: compared as text), groups that are empty or all NULL
SELECT setseed(0.62);
CREATE FUNCTION rnum(p int, s int) RETURNS numeric LANGUAGE sql AS $$
	SELECT CASE WHEN r < 0.05 THEN NULL WHEN r < 0.06 THEN 'NaN'::numeric
	            ELSE (CASE WHEN r < 0.16 THEN repeat('9', 1 + floor(random() * p)::int) ELSE (SELECT string_agg(floor(random() * 10)::int::text, '') FROM generate_series(1, 1 + floor(random() * p)::int)) END)::numeric
	                 * (10::numeric ^ -s) * (CASE WHEN random() < 0.4 THEN -1 ELSE 1 END) END
	FROM (SELECT random() AS r) q $$;
CREATE TABLE rs (id serial, g int, x numeric, y numeric, w numeric(30, 12), v numeric(45, 20)) WITH (autovacuum_enabled = false);
INSERT INTO rs (g, x, y, w, v) SELECT i % 29, rnum(60, floor(random() * 31)::int), rnum(25, floor(random() * 6)::int), rnum(30, 12), rnum(45, 20) FROM generate_series(1, 6000) i;
INSERT INTO rs (g, x, y, w, v) SELECT 50, NULL, NULL, NULL, NULL FROM generate_series(1, 30);
CREATE INDEX rs_col ON rs USING sift_col (g, x, y, w, v);
VACUUM (FREEZE, ANALYZE) rs;
DELETE FROM shapes;
INSERT INTO shapes VALUES
 (1, 'SELECT g, sum(x), avg(x), count(x) FROM rs GROUP BY g'),
 (2, 'SELECT g, sum(y), avg(y) FROM rs GROUP BY g'),
 (3, 'SELECT sum(x), avg(x), sum(w), avg(w), sum(v), avg(v) FROM rs'),
 (4, 'SELECT g, sum(w), avg(w), sum(v), avg(v) FROM rs GROUP BY g'),
 (5, 'SELECT g, sum(x) FILTER (WHERE x < 0), avg(y) FILTER (WHERE y > 0) FROM rs GROUP BY g'),
 (6, 'SELECT g, sum(x), avg(x) FROM rs WHERE g > 40 GROUP BY g'),
 (7, 'SELECT sum(x), avg(x), sum(w), sum(v) FROM rs WHERE g > 1000'),
 (8, 'SELECT g % 3 AS m, sum(x), sum(y), sum(w) FROM rs GROUP BY g % 3');
SELECT n, answer(q, true) = answer(q, false) AS same_answer, used(q) AS scan, answer(q, true) <> '' AS not_empty FROM shapes ORDER BY n;
DELETE FROM rs WHERE id % 7 = 0;
INSERT INTO rs (g, x, y, w, v) SELECT i % 29, rnum(60, floor(random() * 31)::int), rnum(25, floor(random() * 6)::int), rnum(30, 12), rnum(45, 20) FROM generate_series(1, 3000) i;
SELECT n, answer(q, true) = answer(q, false) AS same_answer FROM shapes ORDER BY n;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT n, answer(q, true) = answer(q, false) AS same_answer FROM shapes ORDER BY n;

DROP FUNCTION answer(text, bool), used(text), rnum(int, int);
DROP TABLE shapes, s, rs;
DROP EXTENSION pg_sift;
