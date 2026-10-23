-- A unit that keeps no key list or bitmap of a key_columns leaf answers count(DISTINCT leaf) from the histogram of the cube when the leaf is a cube dim too
--: a list in some units must not make the others scan. 4 units of 7,500 rows: k holds 200 values in a dense range in the
-- first two (a bitmap), and 200 values spread over 2*10^8 in the last two (a list of 800 bytes beyond the 3% of a unit, so none), and the cube has a histogram of
-- k in all four.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '60s';

CREATE TABLE kf (id int, k int, v int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE kf ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kf SELECT i, CASE WHEN i <= 15000 THEN i % 200 ELSE (i % 200) * 1000003 END, i % 1000, repeat('x', 200) FROM generate_series(1, 30000) i;
VACUUM (FREEZE, ANALYZE) kf;
CREATE INDEX kf_col ON kf USING sift_col (id, k, v) WITH (preagg = 'cube(k: v)', key_columns = 'k');

CREATE FUNCTION kf_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION kf_units(q text, OUT units int, OUT pre int, OUT scanned int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
		scanned := coalesce(substring(l from 'Rows Scanned: (\d+)')::int, scanned);
	END LOOP;
END $$;
CREATE TABLE kf_q (n int, shape text, q text);
INSERT INTO kf_q VALUES
	(1, 'distinct',               'SELECT count(DISTINCT k) FROM kf'),
	(2, 'with the sums of v',     'SELECT count(DISTINCT k), count(*), sum(v) FROM kf'),
	(3, 'a test on another leaf', 'SELECT count(DISTINCT k) FROM kf WHERE id <= 20000'),
	(4, 'the key tested',         'SELECT count(DISTINCT k) FROM kf WHERE k < 150'),
	(5, 'in the sparse units',    'SELECT count(DISTINCT k) FROM kf WHERE id > 15000');

-- the answers are the heap's; every unit is answered from the pre-aggregates (the bitmaps of the first two, the histograms of the last two), none scanned
SELECT n, shape, kf_rows(q, true) = kf_rows(q, false) AS same, u.units, u.pre, u.scanned FROM kf_q, LATERAL kf_units(q) u ORDER BY n;

DROP TABLE kf, kf_q;
DROP FUNCTION kf_rows(text, bool), kf_units(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
