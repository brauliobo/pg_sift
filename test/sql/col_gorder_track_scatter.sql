-- A walk of an order array over a table whose rows were updated and not vacuumed asks the tombstone log about the pages of the entries it takes, not about every page of the
-- superblock of each: the keys are spread over the whole heap, so a mask of the superblock made from the log reads the tombstones of the whole table for a top-N of 20 rows.
-- The rows are the heap's, and the log is still the way a page the map does not hold is taken for all-visible.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET synchronous_commit = on;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE gt (id int PRIMARY KEY, u bigint, pad text DEFAULT repeat('x', 60)) WITH (autovacuum_enabled = off);
ALTER TABLE gt ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gt (id, u) SELECT i, (i * 104729::bigint) % 1000003 FROM generate_series(1, 100000) i;
CREATE INDEX gt_i ON gt USING sift_col (id, u) WITH (order_array_columns = 'u', key_cut = 'off');
VACUUM (FREEZE, ANALYZE) gt;
-- 30,000 rows get another key and the old versions stay: tombstones in the log, no VACUUM
UPDATE gt SET u = (u + 5000) % 1000003 WHERE id % 10 < 3;
ANALYZE gt;
SELECT complete, heap_free FROM sift.col_tracking('gt_i');
SELECT (SELECT count(*) FROM sift.col_segments('gt_i') WHERE kind = 'immutable') AS segments, (pg_relation_size('gt') / 8192) > 4 * 256 AS over_four_superblocks;

CREATE FUNCTION gt_rows(q text, via_sift boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', via_sift::text, true), set_config('sift.enable_col_scan', via_sift::text, true), set_config('sift.agg_force', via_sift::text, true),
			set_config('enable_seqscan', (NOT via_sift)::text, true), set_config('enable_indexscan', 'false', true), set_config('enable_bitmapscan', 'false', true),
			set_config('enable_indexonlyscan', 'false', true), set_config('enable_sort', (NOT via_sift)::text, true);
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY ord)) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO r;
	RETURN r;
END $$;
-- the rows through the array and through the heap, the plan, and the pages the walk asked the log about
CREATE FUNCTION gt_check(q text, max_pages int) RETURNS TABLE (global_order boolean, same_rows boolean, pages_it_uses boolean) LANGUAGE plpgsql AS $$
DECLARE a text; b text; asked int8; pl text; walk boolean := false;
BEGIN
	PERFORM sift._test_hf_pages_asked(true);
	a := gt_rows(q, true);
	asked := sift._test_hf_pages_asked();
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP walk := walk OR pl LIKE '%Global Order: true%'; END LOOP;
	b := gt_rows(q, false);
	RETURN QUERY SELECT walk, a = b, asked <= max_pages;
END $$;

SELECT 'offset desc' AS shape, * FROM gt_check('SELECT u FROM gt ORDER BY u DESC LIMIT 20 OFFSET 100', 300);
SELECT 'offset' AS shape, * FROM gt_check('SELECT u FROM gt ORDER BY u LIMIT 20 OFFSET 100', 300);
SELECT 'range' AS shape, * FROM gt_check('SELECT u FROM gt WHERE u BETWEEN 400000 AND 450000 ORDER BY u LIMIT 30', 300);
SELECT 'range desc' AS shape, * FROM gt_check('SELECT u FROM gt WHERE u BETWEEN 400000 AND 450000 ORDER BY u DESC LIMIT 30 OFFSET 40', 300);
-- a deep OFFSET takes thousands of entries: the rows are the heap's whatever the pages
SELECT 'deep offset' AS shape, global_order, same_rows FROM gt_check('SELECT u FROM gt ORDER BY u LIMIT 20 OFFSET 5000', 1000000);
-- after the VACUUM the map holds the pages, and the rows are the same
VACUUM gt;
SELECT 'vacuumed' AS shape, global_order, same_rows FROM gt_check('SELECT u FROM gt ORDER BY u LIMIT 20 OFFSET 100', 1000000);

DROP TABLE gt;
DROP FUNCTION gt_rows(text, boolean), gt_check(text, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
