-- A group array read of a table whose rows were updated and vacuumed takes the exact key of an entry from the heap, and asks of the dead rows of the segment only those of the unit of the
-- entry's page: the top rows of one group are a few pages of a table that has sixty thousand dead rows, and listing them all for each statement was most of its cost.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET synchronous_commit = on;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE gd (id int PRIMARY KEY, g int, ts timestamp, pad text DEFAULT repeat('x', 60)) WITH (autovacuum_enabled = off);
ALTER TABLE gd ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gd (id, g, ts) SELECT i, i % 50, timestamp '2026-01-01' + i * interval '1 minute' FROM generate_series(1, 200000) i;
CREATE INDEX gd_i ON gd USING sift_col (id, g, ts) WITH (order_columns = 'ts', order_array_columns = 'ts', order_array_group = 'g', key_cut = 'off');
VACUUM (FREEZE, ANALYZE) gd;
UPDATE gd SET ts = ts + interval '3 hours', g = (g + 1) % 50 WHERE id % 10 < 3;
VACUUM gd;
SELECT (SELECT count(*) FROM sift.col_segments('gd_i') WHERE kind = 'immutable') AS segments, (SELECT sum(dead_rows) FROM sift.col_segments('gd_i')) > 50000 AS many_dead;

CREATE FUNCTION gd_rows(q text, via_sift boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', via_sift::text, true), set_config('sift.enable_col_scan', via_sift::text, true), set_config('sift.agg_force', via_sift::text, true),
			set_config('enable_seqscan', (NOT via_sift)::text, true), set_config('enable_indexscan', 'false', true), set_config('enable_bitmapscan', 'false', true),
			set_config('enable_indexonlyscan', 'false', true), set_config('enable_sort', (NOT via_sift)::text, true);
	EXECUTE 'SELECT md5(string_agg(x::text, '','' ORDER BY ord)) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO r;
	RETURN r;
END $$;
-- the rows through the array and through the heap, the plan, and the dead addresses the walk listed
CREATE FUNCTION gd_check(q text, max_dead int) RETURNS TABLE (global_order boolean, same_rows boolean, dead_it_lists boolean) LANGUAGE plpgsql AS $$
DECLARE a text; b text; listed int8; pl text; walk boolean := false;
BEGIN
	PERFORM sift._test_probe_dead(true);
	a := gd_rows(q, true);
	listed := sift._test_probe_dead();
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP walk := walk OR pl LIKE '%Global Order: true%'; END LOOP;
	b := gd_rows(q, false);
	RETURN QUERY SELECT walk, a = b, listed <= max_dead;
END $$;

SELECT 'top desc' AS shape, * FROM gd_check('SELECT ts FROM gd WHERE g = 7 ORDER BY ts DESC LIMIT 20', 10000);
SELECT 'top asc' AS shape, * FROM gd_check('SELECT ts FROM gd WHERE g = 8 ORDER BY ts LIMIT 20', 10000);
SELECT 'offset' AS shape, * FROM gd_check('SELECT ts FROM gd WHERE g = 9 ORDER BY ts DESC LIMIT 20 OFFSET 200', 10000);

DROP TABLE gd;
DROP FUNCTION gd_rows(text, boolean), gd_check(text, int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
