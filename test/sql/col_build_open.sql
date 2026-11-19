-- Open units (col/build/open.c): a CREATE INDEX of a sift_col index with key_cut keeps the rows of a value of the key that fills units as units of their own while the table is read, with no
-- temporary file for them; the other values go through the files of the key ranges as before. With no room for the files every range of the key is an open unit. Every path gives the rows of
-- the heap. sift.test_col_open_stats says what the last build did; the mutants plant a defect that a step must catch.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.col_build_source = auto;
SET sift.test_col_segment_kb = 4000;
SET sift.col_unit_rows = 500;

-- op_t: 4 values of the key hold 15% of the rows each (12 units), 3000 hold the rest, in a heap order that is not the key's
CREATE TABLE op_t (id int, g int, k int, note text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO op_t SELECT i, i % 7, CASE WHEN h % 100 < 60 THEN h % 4 ELSE 4 + (h / 100) % 3000 END, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text)
FROM (SELECT i, (hashint4(i)::bigint & 2147483647) AS h FROM generate_series(1, 40000) i) s;
VACUUM (FREEZE, ANALYZE) op_t;

CREATE FUNCTION op_same(t regclass) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; q text;
BEGIN
	FOREACH q IN ARRAY ARRAY[format('SELECT count(*), sum(id), min(note) FROM %s WHERE k = 2', t), format('SELECT g, count(*), sum(id) FROM %s WHERE k BETWEEN 1 AND 40 GROUP BY 1 ORDER BY 1', t),
		format('SELECT count(*), sum(id) FROM %s WHERE k IN (0, 3, 17, 2999)', t), format('SELECT g, count(*) FROM %s GROUP BY 1 ORDER BY 1', t)] LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO b;
		IF a IS DISTINCT FROM b THEN RETURN false; END IF;
	END LOOP;
	RETURN true;
END $$;

-- the units of a heavy value that hold it alone, and of all the units the least such count of a heavy value, by the zones of the index
CREATE FUNCTION op_pure_units() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	SELECT min(c) INTO n FROM (SELECT count(*) FILTER (WHERE min::int = v AND max::int = v) AS c FROM generate_series(0, 3) v, sift.col_unit_agg('op_idx', 'k') GROUP BY v) x;
	RETURN n;
END $$;

\set ci 'CREATE INDEX op_idx ON op_t USING sift_col (id, k, g, note, pad) WITH (key_cut = ''k'', point_tid_columns = ''off'', order_array_columns = ''off'')'
\set verified 'sift.col_verify(''op_idx'', true) IS NOT NULL AS verified'
\set stats 'regexp_replace(current_setting(''sift.test_col_open_stats''), ''(raw|pk|mem|peak|w|u|x)_kb=\\d+'', ''\\1_kb=N'', ''g'') AS stats'

-- the files of the key ranges: the reference
SET sift.test_col_open = -1;
:ci;
SELECT current_setting('sift.test_col_spill_bytes')::int > 0 AS temp_files, op_same('op_t') AS same_rows, op_pure_units() >= 11 AS units_filled, :verified, current_setting('sift.test_col_open_stats') = '' AS no_open_units;
DROP INDEX op_idx;

-- the values that fill units are open units: files only for the others, a heavy value in the units its rows fill
SET sift.test_col_open = 0;
SET client_min_messages = notice;
:ci;
SET client_min_messages = warning;
SELECT current_setting('sift.test_col_spill_bytes')::int > 0 AS temp_files_for_the_others, op_same('op_t') AS same_rows, op_pure_units() >= 11 AS units_filled, :verified;
SELECT :stats;
DROP INDEX op_idx;

-- the off switch
SET sift.col_build_source = heap;
:ci;
SELECT current_setting('sift.test_col_open_stats') = '' AS no_open_units, op_same('op_t') AS same_rows;
DROP INDEX op_idx;
SET sift.col_build_source = auto;

-- no room for the files: every range is an open unit and no file is written
SET temp_file_limit = 64;
SET client_min_messages = notice;
:ci;
SET client_min_messages = warning;
RESET temp_file_limit;
SELECT current_setting('sift.test_col_spill_bytes')::int = 0 AS no_temp_files, op_same('op_t') AS same_rows, :verified;
SELECT :stats;
DROP INDEX op_idx;

-- more heavy values than the open units hold: the largest is written short, answers stay
SET sift.test_col_open_kb = 120;
SET sift.test_col_open_single = 4;
:ci;
SELECT (regexp_match(current_setting('sift.test_col_open_stats'), 'evictions=(\d+)'))[1]::int > 0 AS evicted, op_same('op_t') AS same_rows, :verified;
RESET sift.test_col_open_kb;
RESET sift.test_col_open_single;
DROP INDEX op_idx;

-- a merge puts the units in the order of the key and REINDEX builds the index again
:ci;
SELECT sift.col_merge('op_idx'::regclass, true) IS NOT NULL AS merged;
SELECT op_same('op_t') AS same_rows, op_pure_units() >= 3 AS units_after_merge;
REINDEX INDEX op_idx;
SELECT op_same('op_t') AS same_rows, :verified;
DROP INDEX op_idx;

-- mutants: a defect in the open units, each one caught by what it breaks
SET sift.test_col_open_mutant = 1;
:ci;
SELECT op_pure_units() < 11 AS value_mixed_with_another, op_same('op_t') AS same_rows;
DROP INDEX op_idx;
SET sift.test_col_open_mutant = 2;
:ci;
SELECT NOT op_same('op_t') AS tail_lost;
DROP INDEX op_idx;
SET sift.test_col_open_mutant = 3;
:ci;
SELECT NOT op_same('op_t') AS unit_written_twice;
DROP INDEX op_idx;
SET sift.test_col_open_kb = 120;
SET sift.test_col_open_single = 4;
SET sift.test_col_open_mutant = 4;
:ci;
SELECT (regexp_match(current_setting('sift.test_col_open_stats'), 'evictions=(\d+)'))[1]::int = 0 AS cap_not_looked_at, op_same('op_t') AS same_rows;
DROP INDEX op_idx;
RESET sift.test_col_open_kb;
RESET sift.test_col_open_single;
SET sift.test_col_open_mutant = 5;
:ci;
SELECT count(*) > 20 AS units_that_wait_counted_still FROM sift.col_segments('op_idx');
SELECT op_same('op_t') AS same_rows;
DROP INDEX op_idx;
RESET sift.test_col_open_mutant;

DROP TABLE op_t;
DROP FUNCTION op_same(regclass), op_pure_units();
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
