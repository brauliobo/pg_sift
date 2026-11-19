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

-- the bytes of the writer and of the build do not follow the segments: the largest is under twice the smallest (a merge opens at most 64 inputs at a time; col_merge holds the rows of the one segment it writes: the open scope of test/mem-budgets.tsv)
SELECT stage, scope, max(peak) < 2 * min(peak) AS flat_in_segments FROM peaks WHERE scope <> 'col_merge' GROUP BY stage, scope ORDER BY stage, scope;

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
-- forced merges with a writer budget below the size of their inputs (1 kB against segments of 256 rows) end: the segments a merge wrote as many as it read are settled, so the next one
-- leaves them. The segments a key-cut build makes depend on its estimate of the rows of the table (by ranges, a key in one segment, when the estimate says there are many runs, as a
-- table with statistics does; else in runs that overlap, which sift.test_col_build_runs_overlap asks for): both are built, and no count of segments is asserted
CREATE TABLE u (id int, k int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE u ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO u SELECT i, (i::bigint * 7919) % 1000, repeat('x', 7000) FROM generate_series(1, 3072) i;
SET sift.test_col_segment_kb = 1;
SELECT reltuples < 0 AS no_statistics FROM pg_class WHERE oid = 'u'::regclass;
CREATE INDEX u_fresh ON u USING sift_col (id, k) WITH (key_cut = 'k');
CREATE INDEX u_plain ON u USING sift_col (id, k) WITH (key_cut = 'off');
SELECT 'plain' AS index, count(*) AS built FROM sift.col_segments('u_plain') WHERE kind = 'immutable';
SELECT sift.col_merge('u_plain', true) > 0 AS first_merge;
SELECT count(*) AS after_first, min(rows) AS min_rows, max(rows) AS max_rows FROM sift.col_segments('u_plain') WHERE kind = 'immutable';
SELECT sift.col_merge('u_plain', true) > 0 AS second_merge;
SELECT count(*) AS after_second FROM sift.col_segments('u_plain') WHERE kind = 'immutable';
ANALYZE u;
SELECT reltuples = 3072 AS statistics FROM pg_class WHERE oid = 'u'::regclass;
CREATE INDEX u_cut ON u USING sift_col (id, k) WITH (key_cut = 'k');
SET sift.test_col_build_runs_overlap = on;
CREATE INDEX u_run ON u USING sift_col (id, k) WITH (key_cut = 'k');
RESET sift.test_col_build_runs_overlap;
-- the layout of a build does not depend on the statistics of the table: the segments of the index built before any (the row estimate of the build is its own count of the heap)
SELECT (SELECT array_agg(rows ORDER BY sb_lo, rows) FROM sift.col_segments('u_fresh') WHERE kind = 'immutable') = (SELECT array_agg(rows ORDER BY sb_lo, rows) FROM sift.col_segments('u_cut') WHERE kind = 'immutable') AS same_layout_without_statistics;
-- a unit for each segment; the key range of the table (0 to 999) is covered; a key is in one segment of the build by ranges and in every run of the build in runs
SELECT i AS index, count(*) = (SELECT count(*) FROM sift.col_segments(i) WHERE kind = 'immutable') AS a_unit_each, min(z.min::bigint) = 0 AND max(z.max::bigint) = 999 AS covers_the_keys,
	sum(z.nrows) = 3072 AS all_rows, count(*) FILTER (WHERE z.min::bigint <= 500 AND z.max::bigint >= 500) = CASE i::text WHEN 'u_run' THEN count(*) ELSE 1 END AS key_in_one_range_or_every_run
FROM unnest(ARRAY['u_fresh', 'u_cut', 'u_run']::regclass[]) i, LATERAL sift._test_col_zones(i, 1) z GROUP BY i ORDER BY i::text;
-- forced merges, one statement each (a merge in the transaction of the one before it sees segments whose rows are not final), until one changes nothing: it comes within one more
-- than the segments the index started with, and no row is lost
CREATE TABLE u_log (idx regclass, round int, before int, steps int, after int, rows int8);
CREATE FUNCTION u_round(i regclass, r int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	b int; s int;
BEGIN
	SELECT count(*) INTO b FROM sift.col_segments(i) WHERE kind = 'immutable';
	s = sift.col_merge(i, true);
	INSERT INTO u_log SELECT i, r, b, s, count(*), sum(g.rows) FROM sift.col_segments(i) g WHERE kind = 'immutable';
END $$;
\set ECHO none
\o /dev/null
SELECT format('SELECT u_round(%L, %s)', i, r) FROM unnest(ARRAY['u_plain', 'u_fresh', 'u_cut', 'u_run']::regclass[]) i,
	LATERAL generate_series(1, 1 + (SELECT count(*) FROM sift.col_segments(i) WHERE kind = 'immutable')) r ORDER BY i::text, r
\gexec
\o
\set ECHO all
SELECT idx AS index, bool_or(steps = 0) AS ends, bool_and(rows = 3072) AS all_rows FROM u_log GROUP BY idx ORDER BY idx::text;
SELECT sift.col_verify('u_plain', true);
SELECT sift.col_verify('u_fresh', true);
SELECT sift.col_verify('u_cut', true);
SELECT sift.col_verify('u_run', true);
DROP FUNCTION u_round(regclass, int);
DROP TABLE u_log;
RESET sift.test_col_segment_kb;
DROP TABLE u;
DROP TABLE v, peaks;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
