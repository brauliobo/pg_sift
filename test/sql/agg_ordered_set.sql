-- percentile_cont and percentile_disc of Custom Scan (SiftAgg) keep the values of a group in a histogram and pick the ranks the final functions of PostgreSQL pick, without
-- sorting the histogram (a quickselect of the positions of all the aggregates that share the argument). Every statement is run pushed down and on the normal plan: the rows must be the same,
-- for ties, all-distinct values, NaN, infinities, -0, NULLs, DESC, fractions at the ends, groups of one and two values, aggregates that share an argument and those that do not,
-- a FILTER, a histogram that spills to a tuplesort (work_mem), and parallel workers (their histograms are merged).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
-- 12 digits: a PostgreSQL built with fused multiply-add interpolates in the last bit of a float8 differently
SET extra_float_digits = -3;

CREATE TABLE os (id int, g int, small int, f float8, f4 float4, d date, n int2, pad text DEFAULT repeat('x', 50));
INSERT INTO os (id, g, small, f, f4, d, n)
SELECT i, CASE WHEN i % 11 = 0 THEN 0 WHEN i < 50 THEN i ELSE 1 + i % 37 END, i % 7,
	   CASE WHEN i % 997 = 0 THEN NULL WHEN i % 313 = 0 THEN 'NaN' WHEN i % 331 = 0 THEN 'Infinity' WHEN i % 337 = 0 THEN '-Infinity' WHEN i % 311 = 0 THEN '-0'
			WHEN i % 5 = 0 THEN (i % 13)::float8 ELSE ((i * 7919) % 100003) / 7.0 END,
	   ((i * 31) % 5000) / 3.0, date '2020-01-01' + (i * 7) % 900, (i % 300 - 150)::int2
FROM generate_series(1, 20000) i;
-- groups of one and of two values
INSERT INTO os (id, g, small, f, f4, d, n) VALUES (100001, 9001, 1, 5.5, 1.5, '2024-01-01', 3), (100002, 9002, 1, 1.5, 2.5, '2024-01-02', 1), (100003, 9002, 2, 2.5, 3.5, '2024-01-03', 2);
CREATE INDEX os_col ON os USING sift_col (g, small, f, f4, d, n);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) os;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT t::text AS j FROM (%s) t', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT t::text AS j FROM (%s) t', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE FUNCTION all_same() RETURNS TABLE (n int, plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE q text[] := ARRAY[
	'SELECT g, percentile_cont(0.5) WITHIN GROUP (ORDER BY f) FROM os GROUP BY g',
	'SELECT g, percentile_cont(0) WITHIN GROUP (ORDER BY f), percentile_cont(1) WITHIN GROUP (ORDER BY f), percentile_cont(0.25) WITHIN GROUP (ORDER BY f), percentile_cont(0.999) WITHIN GROUP (ORDER BY f) FROM os GROUP BY g',
	'SELECT small, percentile_cont(0.5) WITHIN GROUP (ORDER BY f DESC), percentile_cont(0.1) WITHIN GROUP (ORDER BY f DESC) FROM os GROUP BY small',
	'SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY f), percentile_disc(0.9) WITHIN GROUP (ORDER BY f DESC) FROM os GROUP BY g',
	'SELECT percentile_cont(0.37) WITHIN GROUP (ORDER BY f), percentile_disc(0.37) WITHIN GROUP (ORDER BY f) FROM os',
	'SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY d), percentile_disc(0.25) WITHIN GROUP (ORDER BY n), percentile_disc(0.75) WITHIN GROUP (ORDER BY f4) FROM os GROUP BY g',
	'SELECT small, percentile_cont(0.5) WITHIN GROUP (ORDER BY f) FILTER (WHERE g < 20), percentile_cont(0.5) WITHIN GROUP (ORDER BY f) FROM os GROUP BY small',
	'SELECT g, percentile_cont(0.5) WITHIN GROUP (ORDER BY f * 2.0 - 1.0), count(*), sum(f4) FROM os WHERE small <> 3 GROUP BY g',
	'SELECT g % 3, percentile_cont(0.5) WITHIN GROUP (ORDER BY f), percentile_cont(0.5) WITHIN GROUP (ORDER BY f4::float8) FROM os WHERE g IN (1, 2, 3, 9001, 9002, 40) GROUP BY 1',
	'SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY small) FROM os GROUP BY g'];
BEGIN
	FOR n IN 1..array_length(q, 1) LOOP
		RETURN QUERY SELECT n, s.plan, s.result_rows, s.vs_normal_plan FROM same(q[n]) s;
	END LOOP;
END $$;

SELECT * FROM all_same();

-- the histogram spills to a tuplesort: the sorted walk, not the quickselect
SET work_mem = '64kB';
SELECT * FROM all_same();
RESET work_mem;

-- parallel workers merge their histograms
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET max_parallel_workers_per_gather = 2;
SELECT * FROM all_same();

RESET ALL;
DROP TABLE os;
DROP FUNCTION all_same(), same(text), plan_of(text);
DROP EXTENSION pg_sift;
