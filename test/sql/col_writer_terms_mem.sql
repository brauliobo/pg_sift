-- The dictionaries and the point arrays of a segment writer are held per SEGMENT, not per table: the bytes of the context of each (col_writer_dict, col_writer_points) are the same for
-- 16,000 rows as for 32,000 while the segments grow from 8 to 32 (a build) and from 2 to 4 (a merge), because a segment is cut when its writer holds its budget and the writer is freed with it.
-- sift.test_col_segment_kb makes the budget 256 kB for the build and 2 MB for the merge. Only a directory too small for the segments the budget asks for (sift.test_col_max_segments = 3)
-- raises the budget to the rows left over the segments left, and then the contexts hold bytes in proportion to the rows of a segment (the open scopes of test/mem-budgets.tsv).
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
SET sift.async_maintenance = off;

CREATE TABLE peaks (regime text, stage text, n int, segs int, per_seg int, scope text, peak bigint);

SELECT s FROM unnest(ARRAY[16000, 20000, 24000, 28000, 32000]) n, unnest(ARRAY['plenty', 'few']) regime, LATERAL unnest(ARRAY[
	'CREATE TABLE ws (id int, g int, k int, u text, pad text DEFAULT repeat(''x'', 1900)) WITH (autovacuum_enabled = off)',
	'ALTER TABLE ws ALTER COLUMN pad SET STORAGE PLAIN',
	format('INSERT INTO ws (id, g, k, u) SELECT i, i %% 50, (i::bigint * 7919) %% 100003, md5(i::text) FROM generate_series(1, %s) i', n),
	'ANALYZE ws',
	format('SET sift.test_col_max_segments = %s', CASE regime WHEN 'few' THEN 3 ELSE 0 END),
	'SET sift.test_col_segment_kb = 256',
	'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)',
	'CREATE INDEX ws_i ON ws USING sift_col (id, g, k, u) WITH (key_cut = ''off'', key_columns = ''k'', order_array_columns = ''id, u'', order_array_group = ''g'')',
	format('INSERT INTO peaks SELECT %L, ''build'', %s, (SELECT count(*) FROM sift.col_segments(''ws_i'')), (SELECT max(rows) FROM sift.col_segments(''ws_i'')), scope, peak
		FROM sift._test_mem_highwater(true) WHERE scope IN (''col_writer_dict'', ''col_writer_points'')', regime, n),
	'UPDATE ws SET g = g + 1 WHERE id % 3 = 0',
	'VACUUM ws',
	'SET sift.test_col_segment_kb = 2048',
	'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)',
	'SELECT sift.col_merge(''ws_i'', true) > 0',
	format('INSERT INTO peaks SELECT %L, ''merge'', %s, (SELECT count(*) FROM sift.col_segments(''ws_i'')), (SELECT max(rows) FROM sift.col_segments(''ws_i'')), scope, peak
		FROM sift._test_mem_highwater(true) WHERE scope IN (''col_writer_dict'', ''col_writer_points'')', regime, n),
	'DROP TABLE ws']) WITH ORDINALITY AS t(s, k) ORDER BY regime, n, k
\gexec

SELECT regime, stage, count(DISTINCT n) AS sizes, count(DISTINCT segs) AS segment_counts, min(segs) >= 2 AS closes_segments FROM peaks GROUP BY regime, stage ORDER BY regime, stage;

-- plenty of segments: the bytes of a context do not follow the rows or the segments, and stay under the budget the writer is cut at
SELECT stage, scope, max(peak) - min(peak) < 2 * min(peak) AS flat, max(peak) < CASE stage WHEN 'build' THEN 256 ELSE 2048 END * 1024 AS under_budget
FROM peaks WHERE regime = 'plenty' GROUP BY stage, scope ORDER BY stage, scope;

-- a directory that holds 3 segments: the dictionary and the arrays of a segment hold the bytes of its rows, 30 to 300 a row (the term that is open)
SELECT scope, min(peak / per_seg) BETWEEN 30 AND 300 AS bytes_a_row, max(peak) > 3 * min(peak) / 2 AS follows_rows FROM peaks WHERE regime = 'few' AND stage = 'build' GROUP BY scope ORDER BY scope;

DROP TABLE peaks;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
