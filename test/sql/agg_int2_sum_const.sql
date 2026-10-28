-- sum(x + k), avg(x - k) and sum(k * x) of a smallint column and an integer constant (int24pl, int42mi, int24mul ..) are answered from the sums of the
-- units as mul * sum + add * count (no value is decoded for a unit the filters keep whole), as the same of a bigint column are: the integer operation cannot raise for
-- any smallint when the constant is within 2^31 - 2^15 (or 65,535 for a product), so the program is the exact 128-bit one, and the sum a bigint as int4_sum gives it.
-- A constant outside that range, a smallint constant (the operator is int2pl, which raises past smallint) and an integer column keep the generic aggregate. The rows are the normal plan's.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE w (id int PRIMARY KEY, g int NOT NULL, x int2, y int4);
SELECT setseed(0.42);
INSERT INTO w SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL WHEN i = 5 THEN -32768 WHEN i = 6 THEN 32767 ELSE (random() * 4000 - 2000)::int2 END, (random() * 1000)::int FROM generate_series(1, 60000) i;
CREATE INDEX w_col ON w USING sift_col (id, g, x, y);
VACUUM (FREEZE, ANALYZE) w;

CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.agg_force = on;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) z;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;
-- the rows the scan read of the table: 0 when the sums of the units answered
CREATE FUNCTION rows_scanned(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	SET LOCAL sift.agg_force = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
	RETURN (SELECT max((n ->> 'Rows Scanned')::bigint) FROM jsonb_path_query(j, 'strict $.**') n WHERE jsonb_typeof(n) = 'object' AND n ? 'Rows Scanned');
END $$;

-- the shapes that run in the scan without the generic aggregate, over NULLs and the two extremes of smallint
SELECT q, differences(q) AS differences, rows_scanned(q) = 0 AS from_the_sums FROM (VALUES
	('SELECT sum(x + 1) AS a, sum(x - 1) AS b, sum(1 - x) AS c, sum(x * 3) AS d, avg(x + 100000) AS e, avg(x * -65535) AS f FROM w'),
	('SELECT sum(2147450879 + x) AS a, sum(x - 2147450879) AS b, sum(-2147450879 + x) AS c FROM w'),
	('SELECT g, sum(x + 5) AS a, avg(x - 5) AS b, count(*) AS n FROM w GROUP BY g'),
	('SELECT g, sum(x + 5) FROM w WHERE y < 100 GROUP BY g'),
	('SELECT sum(x + 5) FILTER (WHERE y < 500) AS a, sum(x + 7) FILTER (WHERE g = 3) AS b FROM w'),
	('SELECT g, sum(x + 5) AS s FROM w GROUP BY g ORDER BY s DESC LIMIT 3'),
	('SELECT sum(x + 5) AS a, sum(x + 6) AS b, sum(x + 7) AS c, sum(x + 8) AS d FROM w WHERE id > 59990')) v(q) ORDER BY q;
-- empty input and an all-NULL group give NULL, as sum() does
SELECT sum(x + 1) AS empty_sum, avg(x + 1) AS empty_avg FROM w WHERE id < 0;
SELECT differences('SELECT sum(x + 1) AS a, avg(x + 1) AS b FROM w WHERE id < 0') AS differences;
-- the same rows without the two extremes, so that PostgreSQL's integer arithmetic does not raise
CREATE TABLE w2 AS SELECT * FROM w WHERE x IS NULL OR x BETWEEN -2000 AND 2000;
CREATE INDEX w2_col ON w2 USING sift_col (id, g, x, y);
VACUUM (FREEZE, ANALYZE) w2;
-- out of the range of the proof, a smallint constant or an integer column: the generic aggregate reads the rows (and raises where PostgreSQL does)
SELECT rows_scanned('SELECT sum(x + 2147450881) FROM w2') > 0 AS over_range, rows_scanned('SELECT sum(x * 65536) FROM w2') > 0 AS product_over_range,
       rows_scanned('SELECT sum(x + 1::int2) FROM w2') > 0 AS smallint_constant, rows_scanned('SELECT sum(y + 1) FROM w2') > 0 AS integer_column;
SET sift.enable_agg = off;
SELECT sum(x + 2147483647) FROM w WHERE id IN (6);
SET sift.enable_agg = on;
SET sift.agg_force = on;
SELECT sum(x + 2147483647) FROM w WHERE id IN (6);
DROP FUNCTION differences(text), rows_scanned(text);
DROP TABLE w, w2;
DROP EXTENSION pg_sift;
