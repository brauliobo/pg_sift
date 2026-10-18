-- The scan of Custom Scan (SiftAgg) over the test column source: workers aggregate the superblocks of the table and the leader combines
-- their groups, units whose zone misses the id range of the query are skipped, count(DISTINCT) is counted per group. In every case the
-- rows are the ones of the normal plan (sift.enable_agg off), and float sums are the same whoever scans.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
LOAD 'pg_sift';

-- fillfactor 10: about six rows a page, five superblocks of 256 pages; id grows with the heap, so a unit holds a narrow range of ids
CREATE TABLE s (id int, g int, small int, wide int, a float8, b float8, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO s
SELECT i, i % 7, i % 50, (i * 7919) % 100003, (i % 64) / 4.0, (i % 1000) * 0.1, NULL
FROM generate_series(1, 8000) i;
CREATE INDEX s_col ON s USING sift_colstub (id, g, small, wide, a, b);
ANALYZE s;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
-- the normal plan of a table this small is cheaper than the scan: leave it only the plan of the reference
SET enable_seqscan = off;

-- a query's workers launched, units read and units pruned, from its EXPLAIN ANALYZE
CREATE FUNCTION scan(q text, workers int, OUT launched int, OUT units int, OUT pruned int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('max_parallel_workers_per_gather', workers::text, false);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		launched := coalesce(substring(l from 'Workers Launched: (\d+)')::int, launched, 0);
		units := coalesce(substring(l from '^ +Units: (\d+)')::int, units);
		pruned := coalesce(substring(l from 'Units Pruned: (\d+)')::int, pruned);
	END LOOP;
END $$;

-- rows that differ between the scan of the first setting (SiftAgg, no workers) and of the second, and the rows of the first
CREATE FUNCTION diffs(q text, agg2 bool, workers2 int, OUT differ bigint, OUT nrows bigint) LANGUAGE plpgsql AS $$
BEGIN
	SET sift.enable_agg = on;
	SET max_parallel_workers_per_gather = 0;
	EXECUTE format('CREATE TEMP TABLE r_a AS %s', q);
	PERFORM set_config('sift.enable_agg', agg2::text, false), set_config('max_parallel_workers_per_gather', workers2::text, false);
	EXECUTE format('CREATE TEMP TABLE r_b AS %s', q);
	SELECT count(*) INTO differ FROM ((TABLE r_a EXCEPT ALL TABLE r_b) UNION ALL (TABLE r_b EXCEPT ALL TABLE r_a)) d;
	SELECT count(*) INTO nrows FROM r_a;
	DROP TABLE r_a, r_b;
	RESET sift.enable_agg;
	RESET max_parallel_workers_per_gather;
END $$;

-- parallel: the plan, the workers that ran, and the same groups as the normal plan
EXPLAIN (COSTS OFF) SELECT g, count(*), sum(small), min(wide) FROM s GROUP BY g;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF) SELECT g, count(*), sum(small), min(wide) FROM s GROUP BY g;
SELECT launched > 0 AS workers_ran, units > 1 AS units_read FROM scan($$SELECT g, count(*), sum(small), min(wide) FROM s GROUP BY g$$, 2);
SELECT * FROM diffs($$SELECT g, count(*) AS n, sum(small) AS sm, min(wide) AS lo, max(wide) AS hi, avg(small) AS av FROM s GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, count(*) AS n, sum(small) AS sm, min(wide) AS lo, max(wide) AS hi, avg(small) AS av FROM s GROUP BY g$$, true, 3);
SELECT * FROM diffs($$SELECT wide % 5003 AS k, count(*) AS n FROM s GROUP BY 1$$, true, 2);

-- float sums that depend on the order of the additions: the same bits with no worker and with any number of them
SELECT * FROM diffs($$SELECT g, sum(b) AS sb, avg(b) AS ab, sum(b * 1.0001) AS sx FROM s GROUP BY g$$, true, 2);
SELECT * FROM diffs($$SELECT g, sum(b) AS sb, avg(b) AS ab, sum(b * 1.0001) AS sx FROM s GROUP BY g$$, true, 4);
SELECT * FROM diffs($$SELECT sum(b) AS sb FROM s$$, true, 3);
-- and the normal plan agrees for sums that do not depend on it
SELECT * FROM diffs($$SELECT g, sum(a) AS sa, avg(a) AS aa FROM s GROUP BY g$$, false, 0);

-- zone pruning: units are skipped by the range of ids of the conjuncts, with and without workers, and the groups are the same
SELECT units + pruned = (SELECT count(*) FROM sift._test_aggstub_units('s_col')) AS all_units, pruned > 0 AS some_pruned
FROM scan($$SELECT g, count(*) FROM s WHERE id BETWEEN 3000 AND 3200 GROUP BY g$$, 0);
SELECT pruned > 0 AS some_pruned, launched > 0 AS workers_ran FROM scan($$SELECT g, count(*) FROM s WHERE id < 1500 GROUP BY g$$, 2);
SELECT pruned = 0 AS none_pruned FROM scan($$SELECT g, count(*) FROM s WHERE id <> 3000 GROUP BY g$$, 0);
SELECT * FROM diffs($$SELECT g, count(*) AS n, sum(small) AS sm FROM s WHERE id BETWEEN 3000 AND 3200 GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, count(*) AS n FROM s WHERE id = 7000 GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, count(*) AS n FROM s WHERE id > 7000 AND id <= 7500 AND g > 1 GROUP BY g$$, true, 2);
SELECT * FROM diffs($$SELECT g, count(*) AS n FROM s WHERE id >= 100000 GROUP BY g$$, false, 0);

-- count(DISTINCT): few values (a bit per value) and many (a hash set), per group and in total, in one process and merged from several
SELECT * FROM diffs($$SELECT g, count(DISTINCT small) AS d, count(DISTINCT wide) AS w, count(*) AS n FROM s GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, count(DISTINCT small) AS d, count(DISTINCT wide) AS w, count(*) AS n FROM s GROUP BY g$$, true, 3);
SELECT * FROM diffs($$SELECT count(DISTINCT small) AS d, count(DISTINCT wide) AS w FROM s$$, true, 2);
SELECT * FROM diffs($$SELECT g, count(DISTINCT wide) FILTER (WHERE id % 3 = 0) AS d FROM s WHERE id > 2000 GROUP BY g$$, true, 2);

-- the aggregates that keep their state in the leader (ordered sets, grouping sets, numeric sums and keys) take no worker, and their rows are the normal plan's
SET max_parallel_workers_per_gather = 2;
EXPLAIN (COSTS OFF) SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY small) AS m FROM s GROUP BY g;
EXPLAIN (COSTS OFF) SELECT g, small % 3 AS k, count(*) AS n FROM s GROUP BY GROUPING SETS ((g), (small % 3), ());
EXPLAIN (COSTS OFF) SELECT g, sum(small::numeric) AS sm FROM s GROUP BY g;
EXPLAIN (COSTS OFF) SELECT small::numeric * g::numeric AS k, count(*) AS n FROM s GROUP BY 1;
RESET max_parallel_workers_per_gather;
SELECT * FROM diffs($$SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY small) AS m, count(*) AS n FROM s GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT g, small % 3 AS k, count(*) AS n FROM s GROUP BY GROUPING SETS ((g), (small % 3), ())$$, false, 0);
SELECT * FROM diffs($$SELECT g, sum(small::numeric) AS sm, avg(small::numeric) AS av FROM s GROUP BY g$$, false, 0);
SELECT * FROM diffs($$SELECT small::numeric * g::numeric AS k, count(*) AS n FROM s GROUP BY 1$$, false, 0);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
