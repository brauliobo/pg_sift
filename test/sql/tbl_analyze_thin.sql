-- ANALYZE of a sift_table gives the statistics of the rows the table holds although it hands the sampler only a share of the rows of a large
-- table (the sampler keeps 30,000 of them): the row count, the distinct values of a small domain, the most common value and the correlation
-- of a column in the order of the table are those of a heap twin
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int, grp int, status text, v int) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE h (LIKE t) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT g, g % 7, CASE WHEN g % 10 < 6 THEN 'paid' WHEN g % 10 < 9 THEN 'sent' ELSE 'lost' END, g FROM generate_series(1, 600000) g;
SELECT sift.table_maintain('t') IS NOT NULL AS maintained;
INSERT INTO h SELECT * FROM t;
DELETE FROM t WHERE id % 4 = 0;
DELETE FROM h WHERE id % 4 = 0;
ANALYZE t, h;
SELECT abs(reltuples - 450000) < 450000 * 0.03 AS rows_within_3_percent FROM pg_class WHERE relname = 't';
SELECT attname, (n_distinct = 7) AS seven_groups, (n_distinct = 3) AS three_statuses
FROM pg_stats WHERE tablename = 't' AND attname IN ('grp', 'status') ORDER BY attname;
SELECT abs((SELECT most_common_freqs[1] FROM pg_stats WHERE tablename = 't' AND attname = 'status')
         - (SELECT most_common_freqs[1] FROM pg_stats WHERE tablename = 'h' AND attname = 'status')) < 0.02 AS paid_share_like_heap,
       abs((SELECT correlation FROM pg_stats WHERE tablename = 't' AND attname = 'id')) > 0.9 AS id_in_table_order;
DROP TABLE t, h;

-- Below 120,000 rows nothing is thinned (the sampler is given every row), and above it the statistics are those of a heap twin all the same: the
-- distinct values of a small domain whose values include the extremes of its type, the most common values and their shares, and the bounds of
-- the histogram of a column whose extremes are held by a few rows (about 0.05% of them: more than a sample misses), at the sizes around the threshold
CREATE TABLE res (n int, what text, ok boolean);
DO $$
DECLARE
	n int;
BEGIN
	FOREACH n IN ARRAY ARRAY[40000, 80000, 119000, 121000] LOOP
		EXECUTE format('CREATE TABLE ts (d date, x int, g int) USING sift_table WITH (autovacuum_enabled = false)');
		EXECUTE format('CREATE TABLE hs (d date, x int, g int) WITH (autovacuum_enabled = false)');
		EXECUTE format($q$INSERT INTO ts SELECT CASE i %% 64 WHEN 0 THEN 'infinity'::date WHEN 1 THEN '-infinity'::date ELSE date '2000-01-01' + (i %% 64) END,
			CASE i %% 2000 WHEN 0 THEN -1000000 WHEN 1000 THEN 1000000 ELSE (i * 7919) %% 1000003 END, i %% 7 FROM generate_series(1, %s) i$q$, n);
		INSERT INTO hs SELECT * FROM ts;
		PERFORM sift.table_maintain('ts');
		ANALYZE ts, hs;
		INSERT INTO res SELECT n, 'rows within 3 percent', abs(reltuples - n) < n * 0.03 FROM pg_class WHERE relname = 'ts';
		INSERT INTO res SELECT n, 'n_distinct of the date domain', (SELECT n_distinct FROM pg_stats WHERE tablename = 'ts' AND attname = 'd')
			= (SELECT n_distinct FROM pg_stats WHERE tablename = 'hs' AND attname = 'd');
		INSERT INTO res SELECT n, 'n_distinct of the groups', (SELECT n_distinct FROM pg_stats WHERE tablename = 'ts' AND attname = 'g') = 7;
		INSERT INTO res SELECT n, 'most common dates', (SELECT array(SELECT unnest(most_common_vals::text::text[]) ORDER BY 1) FROM pg_stats WHERE tablename = 'ts' AND attname = 'd')
			= (SELECT array(SELECT unnest(most_common_vals::text::text[]) ORDER BY 1) FROM pg_stats WHERE tablename = 'hs' AND attname = 'd');
		INSERT INTO res SELECT n, 'share of infinity', abs((SELECT most_common_freqs[array_position(most_common_vals::text::text[], 'infinity')] FROM pg_stats WHERE tablename = 'ts' AND attname = 'd')
			- (SELECT most_common_freqs[array_position(most_common_vals::text::text[], 'infinity')] FROM pg_stats WHERE tablename = 'hs' AND attname = 'd')) < 0.01;
		-- the extremes of x are held by a few rows: most common values of the sample, or bounds of its histogram
		INSERT INTO res SELECT n, 'extremes of x in the statistics', (SELECT min(v::int) = -1000000 AND max(v::int) = 1000000 FROM pg_stats s,
			unnest(coalesce(most_common_vals::text::text[], '{}') || coalesce(histogram_bounds::text::text[], '{}')) v WHERE s.tablename = 'ts' AND s.attname = 'x');
		INSERT INTO res SELECT n, 'extremes of x in the heap twin', (SELECT min(v::int) = -1000000 AND max(v::int) = 1000000 FROM pg_stats s,
			unnest(coalesce(most_common_vals::text::text[], '{}') || coalesce(histogram_bounds::text::text[], '{}')) v WHERE s.tablename = 'hs' AND s.attname = 'x');
		DROP TABLE ts, hs;
	END LOOP;
END $$;
SELECT n, what, ok FROM res ORDER BY n, what;
SELECT count(*) FILTER (WHERE NOT ok) AS failed, count(*) AS checks FROM res;
DROP TABLE res;
DROP EXTENSION pg_sift;
