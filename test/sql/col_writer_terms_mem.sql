-- The dictionaries and the point arrays of a segment writer are held per SEGMENT, not per table: the bytes of the context of each (col_writer_dict, col_writer_points) are the same for
-- 16,000 rows as for 32,000 while the segments grow from 8 to 32 (a build) and from 2 to 3 (a merge), because a segment is cut when its writer holds its budget and the writer is freed with it.
-- sift.test_col_segment_kb makes the budget 256 kB for the build and 2 MB for the merge. The directory has no number of segments the build or the merge fits the rows to: no segment
-- is cut larger than the budget says, so no context holds bytes in proportion to the rows of the table.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
SET sift.async_maintenance = off;

CREATE TABLE peaks (stage text, n int, segs int, per_seg int, scope text, peak bigint);

SELECT s FROM unnest(ARRAY[16000, 20000, 24000, 28000, 32000]) n, LATERAL unnest(ARRAY[
	'CREATE TABLE ws (id int, g int, k int, u text, pad text DEFAULT repeat(''x'', 1900)) WITH (autovacuum_enabled = off)',
	'ALTER TABLE ws ALTER COLUMN pad SET STORAGE PLAIN',
	format('INSERT INTO ws (id, g, k, u) SELECT i, i %% 50, (i::bigint * 7919) %% 100003, md5(i::text) FROM generate_series(1, %s) i', n),
	'ANALYZE ws',
	'SET sift.test_col_segment_kb = 256',
	'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)',
	'CREATE INDEX ws_i ON ws USING sift_col (id, g, k, u) WITH (key_cut = ''off'', key_columns = ''k'', order_array_columns = ''id, u'', order_array_group = ''g'')',
	format('INSERT INTO peaks SELECT ''build'', %s, (SELECT count(*) FROM sift.col_segments(''ws_i'')), (SELECT max(rows) FROM sift.col_segments(''ws_i'')), scope, peak
		FROM sift._test_mem_highwater(true) WHERE scope IN (''col_writer_dict'', ''col_writer_points'')', n),
	'UPDATE ws SET g = g + 1 WHERE id % 3 = 0',
	'VACUUM ws',
	'SET sift.test_col_segment_kb = 2048',
	'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)',
	'SELECT sift.col_merge(''ws_i'', true) > 0',
	format('INSERT INTO peaks SELECT ''merge'', %s, (SELECT count(*) FROM sift.col_segments(''ws_i'')), (SELECT max(rows) FROM sift.col_segments(''ws_i'')), scope, peak
		FROM sift._test_mem_highwater(true) WHERE scope IN (''col_writer_dict'', ''col_writer_points'')', n),
	'DROP TABLE ws']) WITH ORDINALITY AS t(s, k) ORDER BY n, k
\gexec

SELECT stage, count(DISTINCT n) AS sizes, count(DISTINCT segs) AS segment_counts, min(segs) >= 2 AS closes_segments FROM peaks GROUP BY stage ORDER BY stage;

-- the bytes of a context do not follow the rows or the segments, and stay under the budget the writer is cut at
SELECT stage, scope, max(peak) - min(peak) < 2 * min(peak) AS flat, max(peak) < CASE stage WHEN 'build' THEN 256 ELSE 2048 END * 1024 AS under_budget
FROM peaks GROUP BY stage, scope ORDER BY stage, scope;

DROP TABLE peaks;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
