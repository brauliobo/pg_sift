-- The build cuts its segments to fit the rest of the table in the segments the directory has left: after the first segment the budget grows so that 1.25 times the rows of the table
-- less the rows done fit them, about 1.25 * rows_left / segments_left a segment (a directory of S segments: sift.test_col_max_segments). The rows of the table are what the heap's pages have
-- given so far over the pages of the heap, not the planner's estimate: a table never analyzed (reltuples -1), one whose reltuples is 5 times too high and one 5 times too low (pg_class
-- set directly) are cut into the same segments as one analyzed, and the bytes of the writer follow the rows of the segment (the fit of col_writer_terms_mem: 2.2 MB and 160 bytes a row at most).
-- A budget of 2 MB (sift.test_col_segment_kb) keeps the base of the first segment small against it.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
SET sift.async_maintenance = off;

CREATE TABLE fits (estimate text, dir int, segs int, first_rows int, second_rows int, max_rows int, last_rows int, rows_total int, peak bigint);

SELECT s FROM unnest(ARRAY[3, 6]) dir, unnest(ARRAY['unanalyzed', 'analyzed', 'over', 'under']) estimate, LATERAL unnest(ARRAY[
	'CREATE TABLE est (id int, g int, k int, u text, pad text DEFAULT repeat(''x'', 1900)) WITH (autovacuum_enabled = off)',
	'ALTER TABLE est ALTER COLUMN pad SET STORAGE PLAIN',
	'INSERT INTO est (id, g, k, u) SELECT i, i % 50, (i::bigint * 7919) % 100003, md5(i::text) FROM generate_series(1, 96000) i',
	CASE WHEN estimate = 'unanalyzed' THEN 'SELECT 1' ELSE 'ANALYZE est' END,
	CASE estimate WHEN 'over' THEN 'UPDATE pg_class SET reltuples = 5 * 96000 WHERE oid = ''est''::regclass'
				  WHEN 'under' THEN 'UPDATE pg_class SET reltuples = 96000 / 5 WHERE oid = ''est''::regclass' ELSE 'SELECT 1' END,
	format('SET sift.test_col_max_segments = %s', dir),
	'SET sift.test_col_segment_kb = 2048',
	'SELECT count(*) * 0 FROM sift._test_mem_highwater(true)',
	'CREATE INDEX est_i ON est USING sift_col (id, g, k, u) WITH (key_cut = ''off'', key_columns = ''k'', order_array_columns = ''id, u'', order_array_group = ''g'')',
	format('INSERT INTO fits SELECT %L, %s, count(*), (array_agg(rows ORDER BY gen))[1], (array_agg(rows ORDER BY gen))[2], max(rows), (array_agg(rows ORDER BY gen DESC))[1], sum(rows),
		(SELECT peak FROM sift._test_mem_highwater(true) WHERE scope = ''col_build'') FROM sift.col_segments(''est_i'')', estimate, dir),
	'DROP TABLE est']) WITH ORDINALITY AS t(s, k) ORDER BY dir, estimate, k
\gexec

-- every table has all its rows in the segments the directory left room for, whatever the estimate said
SELECT dir, estimate, rows_total = 96000 AS all_rows, segs FROM fits ORDER BY dir, estimate;

-- a segment holds about 1.25 * rows_left / segments_left of the observed rows: the second one (the rows the first left, a quarter over, over the segments that remain) within 15%, and
-- none beyond that (the last one holds what is left, no more than the others do)
SELECT dir, estimate, abs(second_rows - (1.25 * 96000 - first_rows) / (dir - 1)) < 0.15 * (1.25 * 96000 - first_rows) / (dir - 1) AS second_at_model,
	max_rows < 1.15 * (1.25 * 96000 - first_rows) / (dir - 1) AS none_over_model
FROM fits ORDER BY dir, estimate;

-- the writer's bytes follow those rows, not the estimate: the fit of the capacity model, 2.2 MB and 160 bytes a row at most
SELECT dir, estimate, peak < 2200000 + 160 * 1.15 * (1.25 * 96000 - first_rows) / (dir - 1) AS under_envelope FROM fits ORDER BY dir, estimate;

DROP TABLE fits;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
