-- A lookup table joined on `jsonb_build_array(a) = jsonb_build_array(b)` is probed by hash when b is a column of it. When b is a cast of its column
-- (a stable one) there is no key: the scan tests every row of the table for every distinct value of a, and each test builds two jsonb values in
-- PostgreSQL's executor. The scan is priced for the builds, and the normal plan (a merge join) keeps the statement; the keyed form uses the scan.
-- The statements run with enable_agg on and off: the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ev (id int PRIMARY KEY, day date NOT NULL, amount int NOT NULL);
INSERT INTO ev SELECT g, date '2024-01-01' + (g * 7919) % 300, g % 17 FROM generate_series(1, 50000) g;
CREATE INDEX ev_col ON ev USING sift_col (id, day, amount);
VACUUM (FREEZE, ANALYZE) ev;

CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

CREATE FUNCTION days_query(keyed boolean) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT g.d%s AS day, count(e.id) AS n FROM %s LEFT JOIN ev e ON jsonb_build_array(e.day) = jsonb_build_array(g.d%s) GROUP BY 1 ORDER BY 1',
	              CASE WHEN keyed THEN '' ELSE '::date' END,
	              CASE WHEN keyed THEN '(SELECT d::date AS d FROM generate_series(date ''2024-01-01'', date ''2024-10-26'', interval ''1 day'') s(d) OFFSET 0) g'
	                   ELSE 'generate_series(date ''2024-01-01'', date ''2024-10-26'', interval ''1 day'') g(d)' END,
	              CASE WHEN keyed THEN '' ELSE '::date' END)
$$;

SELECT keyed, bool_or(used) AS scan, bool_or(scan_cost < normal_cost) AS scan_cheaper
FROM (VALUES (false), (true)) v(keyed), LATERAL sift.explain_why(days_query(keyed)) GROUP BY keyed ORDER BY keyed;
SELECT differences(days_query(false)) AS differences_unkeyed, differences(days_query(true)) AS differences_keyed;

DROP FUNCTION days_query(boolean), differences(text);
DROP TABLE ev;
DROP EXTENSION pg_sift;
