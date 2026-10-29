-- ORDER BY x LIMIT n over a key chunk that follows the row order (a delta chunk: rows loaded in key order, a counter, a timestamp that only grows) reads a block of
-- 1024 rows at a time from the end the sort wants, not the whole unit; NULL rows that sort first are the first NULL rows. Every shape runs with and without the top-N
-- scan (sift.enable_agg), after every kind of write, and the rows must be the same; "ends" is the number of units the scan read from their ends.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE ue (id int, up int, down int, solid int, rnd int, neg int, dup int, tsn timestamp);
INSERT INTO ue
SELECT g, CASE WHEN g % 7 = 0 THEN NULL ELSE g * 2 END, CASE WHEN g % 5 = 0 THEN NULL ELSE 100000 - g END, g,
	   (hashint4(g) & 2147483647) % 100000, g - 30000, g / 50, timestamp '2024-01-01' + g * interval '1 minute'
FROM generate_series(1, 60000) g;
CREATE INDEX ue_i ON ue USING sift_col (id, up, down, solid, rnd, neg, dup, tsn) WITH (order_array_columns = off);
VACUUM (ANALYZE) ue;

CREATE FUNCTION ue_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
END $$;

-- 'same' or 'DIFFERENT' (the rows and their order), the rows, and the units read from their ends
CREATE FUNCTION ue_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; ends int; j jsonb;
BEGIN
	DROP TABLE IF EXISTS ue_a, ue_b;
	PERFORM ue_mode(true);
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO j;
	ends := coalesce(jsonb_path_query_first(j, '$.**."Units Read From Their Ends"')::int, 0);
	EXECUTE 'CREATE TEMP TABLE ue_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	PERFORM ue_mode(false);
	EXECUTE 'CREATE TEMP TABLE ue_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM ue_a EXCEPT ALL SELECT pos, r FROM ue_b) UNION ALL (SELECT pos, r FROM ue_b EXCEPT ALL SELECT pos, r FROM ue_a)) d;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || ' ' || (SELECT count(*) FROM ue_b) || ' rows, ends ' || ends;
END $$;

CREATE FUNCTION ue_shapes() RETURNS TABLE (shape text, answer text) LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
	FOR r IN
		SELECT * FROM (VALUES
			('up asc 10',                'SELECT id, up FROM ue ORDER BY up LIMIT 10'),
			('up asc nulls first 10',    'SELECT up FROM ue ORDER BY up NULLS FIRST LIMIT 10'),
			('up desc 10',               'SELECT up FROM ue ORDER BY up DESC LIMIT 10'),
			('up desc nulls last 10',    'SELECT id, up FROM ue ORDER BY up DESC NULLS LAST LIMIT 10'),
			('up asc 1',                 'SELECT id, up FROM ue ORDER BY up LIMIT 1'),
			('up asc 2500',              'SELECT id, up FROM ue ORDER BY up LIMIT 2500'),
			('up asc 10 offset 3000',    'SELECT id, up FROM ue ORDER BY up LIMIT 10 OFFSET 3000'),
			('up desc nulls last 3000',  'SELECT id, up FROM ue ORDER BY up DESC NULLS LAST LIMIT 3000'),
			('down asc 10',              'SELECT id, down FROM ue ORDER BY down LIMIT 10'),
			('down desc 10',             'SELECT down FROM ue ORDER BY down DESC LIMIT 10'),
			('down asc nulls first 10',  'SELECT down FROM ue ORDER BY down NULLS FIRST LIMIT 10'),
			('down desc nulls last 10',  'SELECT id, down FROM ue ORDER BY down DESC NULLS LAST LIMIT 10'),
			('solid asc 10',             'SELECT id, solid FROM ue ORDER BY solid LIMIT 10'),
			('solid desc 10',            'SELECT id, solid FROM ue ORDER BY solid DESC LIMIT 10'),
			('neg asc 10',               'SELECT id, neg FROM ue ORDER BY neg LIMIT 10'),
			('neg desc 10',              'SELECT id, neg FROM ue ORDER BY neg DESC LIMIT 10'),
			('dup asc 120 (ties)',       'SELECT dup FROM ue ORDER BY dup LIMIT 120'),
			('dup desc 120 (ties)',      'SELECT dup FROM ue ORDER BY dup DESC LIMIT 120'),
			('tsn asc 10',               'SELECT id, tsn FROM ue ORDER BY tsn LIMIT 10'),
			('tsn desc 10',              'SELECT id, tsn FROM ue ORDER BY tsn DESC LIMIT 10'),
			('rnd asc 10 (no order)',    'SELECT rnd FROM ue ORDER BY rnd LIMIT 10'),
			('solid asc 10 range',       'SELECT id, solid FROM ue WHERE solid > 25000 ORDER BY solid LIMIT 10'),
			('solid desc 10 filter',     'SELECT id, solid FROM ue WHERE id % 97 = 0 ORDER BY solid DESC LIMIT 10'),
			('up asc 10 filter on rnd',  'SELECT id, up FROM ue WHERE rnd < 500 ORDER BY up LIMIT 10'),
			('up asc 10 filter on up',   'SELECT id, up FROM ue WHERE up % 3 = 0 ORDER BY up LIMIT 10'),
			('up desc 10 filter on up',  'SELECT id, up FROM ue WHERE up % 4 = 0 AND id % 5 = 0 ORDER BY up DESC LIMIT 10'),
			('up asc 10 sparse filter',  'SELECT id, up FROM ue WHERE up % 3001 = 0 ORDER BY up LIMIT 10'),
			('down asc 10 or on down',   'SELECT id, down FROM ue WHERE down <= 99990 OR down = 100 ORDER BY down NULLS FIRST LIMIT 200'),
			('two keys solid, id',       'SELECT id, solid FROM ue ORDER BY solid, id LIMIT 10'),
			('two keys up, id desc',     'SELECT id, up FROM ue ORDER BY up DESC, id LIMIT 10')
		) v(n, q)
	LOOP
		shape := r.n;
		answer := ue_check(r.q);
		RETURN NEXT;
	END LOOP;
END $$;

SELECT * FROM ue_shapes();

-- inserts into the write buffer, with keys before and after the unit's and NULLs
INSERT INTO ue SELECT 60000 + g, CASE WHEN g % 3 = 0 THEN NULL ELSE -g END, 200000 + g, 60000 + g, g, 60000 + g, 1200 + g, timestamp '2023-01-01' + g * interval '1 minute' FROM generate_series(1, 300) g;
SELECT * FROM ue_shapes();

-- updates: the best rows move to the other end, the worst to the best
UPDATE ue SET up = 999990 + id, solid = 999990 + id, neg = 999990 + id WHERE id IN (1, 2, 3, 4, 5, 6, 8);
UPDATE ue SET up = -100000 - id, solid = -100000 - id, neg = -100000 - id WHERE id IN (59990, 59991, 59992, 59993);
SELECT * FROM ue_shapes();

-- deletes: the first rows of the unit, then more than the limit of them
DELETE FROM ue WHERE id <= 40;
SELECT * FROM ue_shapes();
DELETE FROM ue WHERE id <= 3000 OR id > 58000;
SELECT * FROM ue_shapes();

VACUUM ue;
SELECT * FROM ue_shapes();
SELECT sift.col_verify('ue_i', true);

-- a promotion writes the units again
SELECT sift.col_promote('ue_i') IS NOT NULL AS promoted;
SELECT * FROM ue_shapes();

DROP FUNCTION ue_shapes(), ue_check(text), ue_mode(bool);
DROP TABLE ue;
DROP EXTENSION pg_sift_test CASCADE;
DROP EXTENSION pg_sift CASCADE;
