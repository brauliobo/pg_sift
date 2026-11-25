-- A scan of a sift_col index of format major 2 counts in pg_stat_user_indexes (idx_scan) as a scan of any other index does, so that a report of unused indexes does not list the index that
-- serves every read: the bitmap scan, the index scan and the index-only scan, the aggregate scan and a rescan of the inner side of a join. The counts of a backend are flushed with
-- pg_stat_force_next_flush, in a statement of their own, before they are read.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE s (id int, grp int, amount int);
INSERT INTO s SELECT i, i % 10, i % 100 FROM generate_series(1, 4000) i;
CREATE INDEX s_col ON s USING sift_col (id, grp, amount) WITH (key_order = 'id');
VACUUM (ANALYZE) s;
CREATE TABLE outer_k (id int);
INSERT INTO outer_k VALUES (3), (5), (7);
ANALYZE outer_k;

CREATE FUNCTION idx_scans() RETURNS bigint LANGUAGE sql AS $$ SELECT idx_scan FROM pg_stat_user_indexes WHERE indexrelname = 's_col' $$;

-- the plan nodes that read the index, so that the shape of each case is part of the output
CREATE FUNCTION s_nodes(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '(Sift|Bitmap Index|Index (Only )?Scan)' THEN RETURN NEXT btrim(l); END IF;
	END LOOP;
END $$;

-- bitmap scan
SELECT pg_stat_reset_single_table_counters('s_col'::regclass);
SET enable_seqscan = off; SET enable_indexscan = off; SET enable_indexonlyscan = off; SET sift.enable_agg = off;
SELECT s_nodes($$SELECT count(*) FROM s WHERE id BETWEEN 10 AND 30$$);
SELECT count(*) FROM s WHERE id BETWEEN 10 AND 30;
SELECT pg_stat_force_next_flush();
SELECT idx_scans() AS bitmap_scan;

-- index scan
SELECT pg_stat_reset_single_table_counters('s_col'::regclass);
RESET enable_indexscan; SET enable_bitmapscan = off;
SELECT s_nodes($$SELECT amount FROM s WHERE id = 77$$);
SELECT amount FROM s WHERE id = 77;
SELECT pg_stat_force_next_flush();
SELECT idx_scans() AS index_scan;

-- index-only scan
SELECT pg_stat_reset_single_table_counters('s_col'::regclass);
RESET enable_indexonlyscan;
SELECT s_nodes($$SELECT id, grp FROM s WHERE id BETWEEN 100 AND 120$$);
SELECT count(*) FROM (SELECT id, grp FROM s WHERE id BETWEEN 100 AND 120) x;
SELECT pg_stat_force_next_flush();
SELECT idx_scans() AS index_only_scan;

-- the aggregate scan
SELECT pg_stat_reset_single_table_counters('s_col'::regclass);
RESET enable_bitmapscan; RESET enable_seqscan; SET sift.agg_force = on; SET sift.enable_agg = on;
SELECT s_nodes($$SELECT grp, sum(amount) FROM s GROUP BY grp$$);
SELECT count(*) FROM (SELECT grp, sum(amount) FROM s GROUP BY grp) x;
SELECT pg_stat_force_next_flush();
SELECT idx_scans() AS aggregate_scan;

-- one scan for each probe of the inner side of a nested loop
SELECT pg_stat_reset_single_table_counters('s_col'::regclass);
RESET sift.agg_force; SET sift.enable_agg = off; SET enable_seqscan = off; SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_bitmapscan = off;
SELECT s_nodes($$SELECT count(*) FROM outer_k o JOIN s ON s.id = o.id$$);
SELECT count(*) FROM outer_k o JOIN s ON s.id = o.id;
SELECT pg_stat_force_next_flush();
SELECT idx_scans() AS nested_loop_probes;

DROP TABLE s, outer_k;
DROP FUNCTION idx_scans(), s_nodes(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
