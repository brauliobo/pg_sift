-- The ids of the integer keys of a group (agg/intids.c): a table of ids that is pages while the keys are in a small or dense range and a hash otherwise.
-- The table against a plain hash over many shapes of keys, and the groups of keys of every integer type (negative, NULL, the ends of int64, dense and
-- sparse, an expression of a column, a numeric expression beyond int64, NaN and -0 floats) equal to the normal plan's rows.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION pg_sift_test;

SELECT bad, paged > 0 AS paged, hashed > 0 AS hashed, up > 0 AS up, down > 0 AS down FROM sift._test_intids(7, 400);

SET max_parallel_workers_per_gather = 0;
SET work_mem = '256MB';

CREATE TABLE d (id int, i4 int, i8 bigint, sp bigint, ext bigint, dt date, ts timestamptz, f8 float8, nu numeric(12,2), v int) WITH (fillfactor = 20, autovacuum_enabled = false);
INSERT INTO d SELECT i,
	(i * 37 % 700) - 350,
	CASE WHEN i % 11 = 0 THEN NULL ELSE 4000000000 + (i * 37 % 5000) END,
	i::bigint * 1000003 * 7919,
	CASE i % 4 WHEN 0 THEN 9223372036854775807 WHEN 1 THEN -9223372036854775808 WHEN 2 THEN 9223372036854775800 + (i % 5) ELSE -9223372036854775800 - (i % 5) END,
	date '2020-01-01' + (i % 900),
	timestamptz '2020-01-01 00:00:00+00' + (i * 61 % 20000) * interval '1 second',
	CASE i % 7 WHEN 0 THEN 'NaN'::float8 WHEN 1 THEN '-0'::float8 WHEN 2 THEN 0 WHEN 3 THEN 'Infinity'::float8 WHEN 4 THEN '-Infinity'::float8 ELSE (i % 50) * 0.5 END,
	(i % 3000) / 100.0,
	(i * 7) % 1000
FROM generate_series(1, 12000) i;
CREATE INDEX d_col ON d USING sift_col (id, i4, i8, sp, ext, dt, ts, f8, nu, v);
VACUUM ANALYZE d;

CREATE FUNCTION d_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT md5(string_agg(q::text, E''\n'' ORDER BY q::text)) || '' '' || count(*) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION d_scan(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_has_siftagg_pushdown_on(q) $$;

CREATE TABLE d_q (n int, q text);
INSERT INTO d_q VALUES
	(1, 'SELECT i4, count(*) AS n, sum(v) AS s FROM d GROUP BY i4'),
	(2, 'SELECT i8, count(*) AS n, sum(v) AS s FROM d GROUP BY i8'),
	(3, 'SELECT sp, count(*) AS n, sum(v) AS s FROM d GROUP BY sp'),
	(4, 'SELECT ext, count(*) AS n, sum(v) AS s FROM d GROUP BY ext'),
	(5, 'SELECT dt, count(*) AS n, sum(v) AS s FROM d GROUP BY dt'),
	(6, 'SELECT ts, count(*) AS n, sum(v) AS s FROM d GROUP BY ts'),
	(7, 'SELECT f8, count(*) AS n, sum(v) AS s FROM d GROUP BY f8'),
	(8, 'SELECT nu, count(*) AS n, sum(v) AS s FROM d GROUP BY nu'),
	(9, 'SELECT i8 / 3 AS g, count(*) AS n, sum(v) AS s FROM d GROUP BY 1'),
	(10, 'SELECT i4 / 2 AS g, count(*) AS n, sum(v) AS s FROM d GROUP BY 1'),
	(11, 'SELECT i4 % 100 AS g, count(*) AS n, sum(v) AS s FROM d GROUP BY 1'),
	(12, 'SELECT ext / 4 AS g, count(*) AS n, sum(v) AS s FROM d GROUP BY 1'),
	(13, 'SELECT sp % 997 AS g, count(*) AS n, sum(v) AS s FROM d GROUP BY 1'),
	(14, 'SELECT dt - 5 AS g, count(*) AS n, sum(v) AS s FROM d GROUP BY 1'),
	(15, 'SELECT date_trunc(''hour'', ts) AS g, count(*) AS n, sum(v) AS s FROM d GROUP BY 1'),
	(16, 'SELECT i4, i8, count(*) AS n, sum(v) AS s FROM d GROUP BY i4, i8'),
	(17, 'SELECT i8 / 3 AS g, i4 / 2 AS h, count(*) AS n, min(v) AS lo FROM d GROUP BY 1, 2'),
	(18, 'SELECT i8 / 3 AS g, count(*) AS n FROM d WHERE id <= 3000 GROUP BY 1'),
	(19, 'SELECT i4, count(DISTINCT i8) AS n FROM d GROUP BY i4'),
	(20, 'SELECT ext / 4 AS g, count(*) AS n FROM d WHERE id % 3 = 0 GROUP BY 1'),
	(21, 'SELECT nu * 100000000000000000000 AS g, count(*) AS n, sum(v) AS s FROM d GROUP BY 1'),
	(22, 'SELECT ext * 4.0 AS g, count(*) AS n FROM d GROUP BY 1');

-- the plan is the scan, and the rows are the normal plan's
SELECT n, d_scan(q) AS scan, d_rows(q, true) = d_rows(q, false) AS same FROM d_q ORDER BY n;

-- the groups in hash mode (the arrays of group ids forced too small), whatever the table of key ids
SET sift.test_direct_max = 64;
SELECT n, d_rows(q, true) = d_rows(q, false) AS hash FROM d_q ORDER BY n;
RESET sift.test_direct_max;

DROP TABLE d_q;
DROP FUNCTION d_rows(text, bool), d_scan(text);
DROP TABLE d;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
