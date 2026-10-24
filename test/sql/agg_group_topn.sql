-- ORDER BY an aggregate LIMIT n over the groups of a scan: the scan drops the groups that rank after the n-th of the first sort key before they become
-- tuples, and PostgreSQL's sort and limit above keep the rest. Every query runs pushed down and on the normal plan: the rows must be the same, for sums
-- and averages of every kind, counts, ASC and DESC, NULLS FIRST and LAST, a second sort key, an OFFSET, NaN and infinities, groups that tie with the
-- n-th (the key values of the rows must be the same, the groups among the ties may differ), and queries the pushdown must leave alone.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
-- 12 digits: the sums of floats are added in another order than the normal plan's
SET extra_float_digits = -3;

CREATE TABLE gt (id int PRIMARY KEY, g int, tie int, small int, f float8, n numeric(12,2), q int2, pad text DEFAULT repeat('x', 40));
INSERT INTO gt (id, g, tie, small, f, n, q)
SELECT i, (i * 7919) % 3000, i % 2500, i % 5,
       CASE WHEN i % 997 = 0 THEN NULL WHEN i % 1999 = 0 THEN 'NaN' WHEN i % 1511 = 0 THEN 'Infinity' ELSE ((i * 31) % 1009) / 7.0 END,
       CASE WHEN i % 991 = 0 THEN NULL ELSE ((i * 17) % 10007 - 5000) / 100.0 END,
       CASE WHEN i % 13 = 0 THEN NULL ELSE (i % 200 - 100)::int2 END
FROM generate_series(1, 30000) i;
-- groups whose sums are all NULL, and groups of one row
INSERT INTO gt (id, g, tie, small, f, n, q) SELECT 40000 + i, 5000 + i, 1, 0, NULL, NULL, NULL FROM generate_series(1, 40) i;
CREATE INDEX gt_col ON gt USING sift_col (id, g, tie, small, f, n, q);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) gt;

CREATE FUNCTION pushed(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || q LOOP
		IF l ~ 'Groups Dropped By Top-N: [1-9]' THEN r := 'dropped'; ELSIF l LIKE '%Custom Scan (SiftAgg)%' AND r = 'normal' THEN r := 'SiftAgg'; END IF;
	END LOOP;
	RETURN r;
END $$;

-- exact: the whole ordering is decided by the sort keys
CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := pushed(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT row_number() OVER () AS pos, t::text AS j FROM (%s) t', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT row_number() OVER () AS pos, t::text AS j FROM (%s) t', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- ties at the boundary: only the values of the sort key must be those of the normal plan
CREATE FUNCTION same_keys(q text, keycol text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := pushed(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT row_number() OVER () AS pos, %s::text AS j FROM (%s) t', keycol, q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT row_number() OVER () AS pos, %s::text AS j FROM (%s) t', keycol, q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE FUNCTION all_same() RETURNS TABLE (n int, plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE q text[] := ARRAY[
	'SELECT g, count(*) v FROM gt GROUP BY g ORDER BY v DESC, g LIMIT 10',
	'SELECT g, count(*) v FROM gt GROUP BY g ORDER BY v, g LIMIT 7 OFFSET 3',
	'SELECT g, sum(q) v FROM gt GROUP BY g ORDER BY v DESC NULLS LAST, g LIMIT 10',
	'SELECT g, sum(q) v FROM gt GROUP BY g ORDER BY v DESC NULLS FIRST, g LIMIT 10',
	'SELECT g, sum(q) v FROM gt GROUP BY g ORDER BY v ASC NULLS FIRST, g LIMIT 10',
	'SELECT g, sum(q) v FROM gt GROUP BY g ORDER BY v ASC NULLS LAST, g LIMIT 10 OFFSET 20',
	'SELECT g, sum(n) v FROM gt GROUP BY g ORDER BY v DESC, g LIMIT 15',
	'SELECT g, sum(n) v FROM gt GROUP BY g ORDER BY v, g LIMIT 15',
	'SELECT g, sum(n * 3) v, count(n) c FROM gt GROUP BY g ORDER BY v DESC NULLS LAST, c, g LIMIT 12',
	'SELECT g, avg(f) v FROM gt GROUP BY g ORDER BY v DESC NULLS LAST, g LIMIT 10',
	'SELECT g, sum(f) v FROM gt GROUP BY g ORDER BY v DESC NULLS LAST, g LIMIT 10',
	'SELECT g, count(DISTINCT tie) v FROM gt GROUP BY g ORDER BY v DESC, g LIMIT 10',
	'SELECT id, sum(n) v FROM gt GROUP BY id ORDER BY v DESC NULLS LAST, id LIMIT 10',
	'SELECT g, small, sum(n) v FROM gt GROUP BY g, small ORDER BY v DESC NULLS LAST, g, small LIMIT 10',
	'SELECT g, sum(n) v FROM gt WHERE small < 3 GROUP BY g ORDER BY v DESC NULLS LAST, g LIMIT 10',
	'SELECT g, sum(n) v, sum(q) w FROM gt GROUP BY g ORDER BY w DESC NULLS LAST, v, g LIMIT 10',
	'SELECT g, sum(n) v FROM gt GROUP BY g ORDER BY v DESC NULLS LAST, g LIMIT 3000',
	'SELECT g, sum(n) v FROM gt GROUP BY g ORDER BY v DESC NULLS LAST, g LIMIT 1',
	'SELECT g, sum(n) v FROM gt GROUP BY g ORDER BY v DESC NULLS LAST, g LIMIT 0'];
BEGIN
	FOR n IN 1..array_length(q, 1) LOOP
		RETURN QUERY SELECT n, s.plan, s.result_rows, s.vs_normal_plan FROM same(q[n]) s;
	END LOOP;
END $$;

SELECT * FROM all_same();

-- ties with the n-th (a float sum of tied groups differs in the last bit between orders of addition): any of the tied groups may be returned, the values of the key are the same
SELECT 'count' AS what, * FROM same_keys('SELECT g, count(*) v FROM gt GROUP BY g ORDER BY v DESC LIMIT 20', 'v');
SELECT 'count_offset' AS what, * FROM same_keys('SELECT g, count(*) v FROM gt GROUP BY g ORDER BY v LIMIT 15 OFFSET 10', 'v');
SELECT 'sum_int' AS what, * FROM same_keys('SELECT tie, sum(small) v FROM gt GROUP BY tie ORDER BY v DESC LIMIT 25', 'v');
SELECT 'sum_float' AS what, * FROM same_keys('SELECT g, sum(f) v FROM gt WHERE f < ''Infinity'' GROUP BY g ORDER BY v DESC, g LIMIT 10', 'v');
SELECT 'avg_float' AS what, * FROM same_keys('SELECT g, avg(f) v FROM gt WHERE f <> ''NaN'' GROUP BY g ORDER BY v, g LIMIT 10', 'v');
SELECT 'avg' AS what, * FROM same_keys('SELECT tie, avg(f) v FROM gt WHERE f < ''Infinity'' GROUP BY tie ORDER BY v LIMIT 25', 'v');

-- left alone: a HAVING, a window, DISTINCT, grouping sets, a first key that is not an aggregate, an expression of aggregates, a text sum
SELECT 'having' AS what, * FROM same('SELECT g, sum(n) v FROM gt GROUP BY g HAVING count(*) > 5 ORDER BY v DESC NULLS LAST, g LIMIT 10');
SELECT 'key_first' AS what, * FROM same('SELECT g, sum(n) v FROM gt GROUP BY g ORDER BY g DESC LIMIT 10');
SELECT 'expression' AS what, * FROM same('SELECT g, sum(n) + count(*) v FROM gt GROUP BY g ORDER BY v DESC NULLS LAST, g LIMIT 10');
SELECT 'rollup' AS what, * FROM same('SELECT g, sum(n) v FROM gt GROUP BY ROLLUP (g) ORDER BY v DESC NULLS LAST, g LIMIT 10');
SELECT 'with_ties' AS what, * FROM same_keys('SELECT g, count(*) v FROM gt GROUP BY g ORDER BY v DESC FETCH FIRST 10 ROWS WITH TIES', 'v');
SELECT 'window' AS what, * FROM same('SELECT g, v, rank() OVER (ORDER BY v DESC NULLS LAST, g) r FROM (SELECT g, sum(n) v FROM gt GROUP BY g) s ORDER BY r LIMIT 10');

-- parallel workers merge their groups before the top-N
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET max_parallel_workers_per_gather = 2;
SELECT * FROM all_same();

RESET ALL;
DROP TABLE gt;
DROP FUNCTION all_same(), same(text), same_keys(text, text), pushed(text);
DROP EXTENSION pg_sift;
