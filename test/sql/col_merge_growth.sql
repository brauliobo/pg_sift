-- A forced merge never writes more segments than it read, unless its writer budget makes a segment of each unit it holds: then at most as many as the bytes make of the budget.
-- A merge cuts a segment at a unit once its writer holds the budget, and a last piece of under the slack of the one before it joins it, so the rows a cut at a unit leaves over
-- (units of a key range are a little under or over the aim) are not a segment of their own, which the next merge would have to merge again.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.async_maintenance = off;
SET maintenance_work_mem = '64MB';
SET statement_timeout = '120s';

CREATE TABLE u (id int, k int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE u ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO u SELECT i, (i::bigint * 7919) % 1000, repeat('x', 7000) FROM generate_series(1, 3072) i;
ANALYZE u;

CREATE TABLE growth (layout text, budget_kb int, ins int, outs int, ends boolean, bytes bigint, rows bigint);

-- layout: runs (key_cut, 12 overlapping runs of 256 rows), ranges (key_cut, a key in one segment), plain (no key_cut), coarse (key_cut, built at 16 kB: segments a smaller budget cuts again); budget_kb 0: the default. Forced merges, one statement each, until one changes nothing
CREATE FUNCTION grow(layout text, budget_kb int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	b int;
	s int;
BEGIN
	EXECUTE 'DROP INDEX IF EXISTS g_i';
	PERFORM set_config('sift.test_col_segment_kb', CASE layout WHEN 'coarse' THEN '16' ELSE '1' END, false);
	PERFORM set_config('sift.test_col_build_runs_overlap', CASE WHEN layout = 'runs' THEN 'on' ELSE 'off' END, false);
	EXECUTE format('CREATE INDEX g_i ON u USING sift_col (id, k) WITH (key_cut = %L, compression = ''none'')', CASE WHEN layout = 'plain' THEN 'off' ELSE 'k' END);
	PERFORM set_config('sift.test_col_build_runs_overlap', 'off', false);
	PERFORM set_config('sift.test_col_segment_kb', budget_kb::text, false);
	SELECT count(*) INTO b FROM sift.col_segments('g_i') WHERE kind = 'immutable';
	FOR i IN 1..b + 2 LOOP
		s = sift.col_merge('g_i', true);
		EXIT WHEN s = 0;
	END LOOP;
	INSERT INTO growth SELECT layout, budget_kb, b, count(*), s = 0, sum(bytes), sum(rows) FROM sift.col_segments('g_i') WHERE kind = 'immutable';
END $$;

\set ECHO none
\o /dev/null
SELECT grow(l, kb) FROM unnest(ARRAY['runs', 'ranges', 'plain', 'coarse']) l, unnest(ARRAY[1, 4, 16, 64, 0]) kb ORDER BY l, kb;
\o
\set ECHO all

-- the rows are kept, and the forced merges end
SELECT layout, budget_kb, rows = 3072 AS all_rows, ends FROM growth WHERE rows <> 3072 OR NOT ends ORDER BY layout, budget_kb;
-- a merge of segments that each exceed the budget writes no more than it read
SELECT layout, budget_kb, ins, outs FROM growth WHERE layout <> 'coarse' AND outs > ins ORDER BY layout, budget_kb;
-- segments over the budget are cut again, a segment to each unit of 256 rows at most (3,072 rows: 12), within the bytes the budget makes
SELECT layout, budget_kb, ins, outs, outs <= 12 AS a_unit_each_at_most, outs <= ceil(bytes / (CASE budget_kb WHEN 0 THEN 65536 ELSE budget_kb END * 1024.0)) AS within_budget FROM growth WHERE layout = 'coarse' ORDER BY budget_kb;
-- every layout at every budget is within the bound: at most as many as it read, or as the bytes make of the budget
SELECT count(*) AS cases, count(*) FILTER (WHERE outs > greatest(ins, ceil(bytes / (CASE budget_kb WHEN 0 THEN 65536 ELSE budget_kb END * 1024.0)))) AS above_the_bound FROM growth;
SELECT sift.col_verify('g_i', true);

DROP FUNCTION grow(text, int);
DROP TABLE growth, u;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
