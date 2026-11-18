-- A sift_col build or merge has no number of segments it fits the rows to: a table whose rows fill more segments than the 128 the writers of the other index kinds keep a directory within
-- gets them, each cut at the budget of its writer, and the writer holds the same bytes whether the table makes 44 segments or 131. sift.test_col_segment_kb = 1 cuts a segment at
-- each superblock (256 heap pages: a row of 7 kB to the page, 256 rows to a segment); the directory of 131 is a chain, takes writes without a merge to make room, and a forced
-- merge of it writes fewer, with the bytes of its writer flat in the segments it reads.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.col_auto_merge = off;
SET sift.async_maintenance = off;
SET maintenance_work_mem = '64MB';
SET statement_timeout = '120s';

CREATE TABLE peaks (stage text, n int, segs int, min_rows int, max_rows int, scope text, peak bigint);

SELECT s FROM unnest(ARRAY[11264, 22528, 33536]) n, LATERAL unnest(ARRAY[
	'CREATE TABLE w (id int, k int, pad text) WITH (autovacuum_enabled = off, fillfactor = 100)',
	'ALTER TABLE w ALTER COLUMN pad SET STORAGE PLAIN',
	format('INSERT INTO w SELECT i, (i::bigint * 7919) %% 1000, repeat(''x'', 7000) FROM generate_series(1, %s) i', n),
	'SET sift.test_col_segment_kb = 1',
	'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)',
	'CREATE INDEX w_i ON w USING sift_col (id, k) WITH (key_cut = ''off'')',
	format('INSERT INTO peaks SELECT ''build'', %s, count(*), min(rows), max(rows), scope, peak FROM sift.col_segments(''w_i'') s,
		(SELECT scope, peak FROM sift._test_mem_highwater(true) WHERE scope IN (''col_build'', ''col_writer_dict'', ''col_writer_points'')) m WHERE kind = ''immutable'' GROUP BY scope, peak', n),
	'RESET sift.test_col_segment_kb',
	'SELECT count(*) * 0 AS reset FROM sift._test_mem_highwater(true)',
	'SELECT sift.col_merge(''w_i'', true) > 0',
	format('INSERT INTO peaks SELECT ''merge'', %s, count(*), min(rows), max(rows), scope, peak FROM sift.col_segments(''w_i'') s,
		(SELECT scope, peak FROM sift._test_mem_highwater(true) WHERE scope IN (''col_merge'', ''col_merge_inputs'', ''col_writer_dict'', ''col_writer_points'')) m WHERE kind = ''immutable'' GROUP BY scope, peak', n),
	'DROP TABLE w']) WITH ORDINALITY AS t(s, k) ORDER BY n, k
\gexec

-- every segment of the build is one superblock, and the largest table makes more than the policy of the other kinds (128)
SELECT n, segs, min_rows = 256 AND max_rows = 256 AS one_superblock_each, segs > 128 AS past_the_policy FROM peaks WHERE stage = 'build' AND scope = 'col_build' ORDER BY n;

-- the bytes of the writer and of the build do not follow the segments: the largest is under twice the smallest (the merge opens every input, 8 kB of col_merge and col_merge_inputs for each: the open scopes of test/mem-budgets.tsv)
SELECT stage, scope, max(peak) < 2 * min(peak) AS flat_in_segments FROM peaks WHERE scope NOT IN ('col_merge', 'col_merge_inputs') GROUP BY stage, scope ORDER BY stage, scope;

-- the merge of 44, 88 and 131 segments writes the one segment their rows make
SELECT n, segs = 1 AS one_segment FROM peaks WHERE stage = 'merge' AND scope = 'col_merge' ORDER BY n;

-- the largest index takes writes with its 131 segments: the buffers it seals are promoted and no merge makes room (the other kinds merge from 112)
CREATE TABLE v (id int, k int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE v ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO v SELECT i, i % 1000, repeat('x', 7000) FROM generate_series(1, 33536) i;
SET sift.test_col_segment_kb = 1;
CREATE INDEX v_i ON v USING sift_col (id, k) WITH (key_cut = 'off', write_buffer_limit = 64);
RESET sift.test_col_segment_kb;
SELECT count(*) AS segments FROM sift.col_segments('v_i') WHERE kind = 'immutable';
INSERT INTO v SELECT i, i % 1000, 'y' FROM generate_series(33537, 133536) i;
SELECT sift.col_promote('v_i') >= 0 AS promoted;
SELECT count(*) FILTER (WHERE kind = 'immutable') > 131 AS grown_past_131, count(*) FILTER (WHERE kind = 'immutable') > 128 AS past_the_policy FROM sift.col_segments('v_i');
SELECT sift.col_verify('v_i', true);
SELECT count(*) AS rows_found FROM v WHERE k < 1000 AND id > 0;
DROP TABLE v, peaks;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
