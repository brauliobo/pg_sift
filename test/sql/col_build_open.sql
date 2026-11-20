-- Open units (col/build/open.c): a CREATE INDEX of a sift_col index with key_cut that has no room for the temporary files of the key ranges makes every range of the key an open unit: the rows
-- are packed in memory and written as units as they fill, and no file is written, where the build was refused before. With room for the files they are used. Every path gives the rows of the heap.
-- sift.test_col_open_stats says what the last build did; sift.test_col_open = 1 forces the open units, and the mutants plant a defect that a step must catch.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.col_build_source = auto;
SET sift.col_unit_rows = 100;
SET sift.test_col_segment_kb = 4000;

-- op_m: four values of the key hold 38% of the rows (38 units each), 3000 the rest; the heap order is not the key's
CREATE TABLE op_m (id int, g int, k int, note text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO op_m SELECT i, i % 7, CASE WHEN h % 100 < 38 THEN h % 4 ELSE 4 + (h / 100) % 3000 END, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text)
FROM (SELECT i, (hashint4(i)::bigint & 2147483647) AS h FROM generate_series(1, 40000) i) s;
VACUUM (FREEZE, ANALYZE) op_m;

CREATE FUNCTION op_same(t regclass) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; q text;
BEGIN
	FOREACH q IN ARRAY ARRAY[format('SELECT count(*), sum(id), min(note) FROM %s WHERE k = 1', t), format('SELECT g, count(*), sum(id) FROM %s WHERE k BETWEEN 0 AND 40 GROUP BY 1 ORDER BY 1', t),
		format('SELECT count(*), sum(id) FROM %s WHERE k IN (0, 1, 17, 2999)', t), format('SELECT g, count(*) FROM %s GROUP BY 1 ORDER BY 1', t)] LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO b;
		IF a IS DISTINCT FROM b THEN RETURN false; END IF;
	END LOOP;
	RETURN true;
END $$;

-- the least count, over the values 0 to vmax, of the units that hold one value alone, by the zones of the index
CREATE FUNCTION op_pure_units(vmax int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	SELECT min(c) INTO n FROM (SELECT count(*) FILTER (WHERE min::int = v AND max::int = v) AS c FROM generate_series(0, vmax) v, sift.col_unit_agg('op_idx', 'k') GROUP BY v) x;
	RETURN n;
END $$;

\set ci 'CREATE INDEX op_idx ON op_m USING sift_col (id, k, g, note, pad) WITH (key_cut = ''k'', point_tid_columns = ''off'', order_array_columns = ''off'')'
\set verified 'sift.col_verify(''op_idx'', true) IS NOT NULL AS verified'
\set stats 'regexp_replace(current_setting(''sift.test_col_open_stats''), ''(raw|pk|mem|peak|w|u|x)_kb=\\d+'', ''\\1_kb=N'', ''g'') AS stats'

-- with room for the files the key ranges take the rows and nothing is open
SET sift.test_col_open = 0;
:ci;
SELECT current_setting('sift.test_col_spill_bytes')::int > 0 AS temp_files, current_setting('sift.test_col_open_stats') = '' AS no_open_units, op_same('op_m') AS same_rows, op_pure_units(3) >= 35 AS units_filled, :verified;
DROP INDEX op_idx;

-- no room for the files: every range is an open unit and no file is written
SET temp_file_limit = 64;
SET client_min_messages = notice;
:ci;
SET client_min_messages = warning;
SELECT current_setting('sift.test_col_spill_bytes')::int = 0 AS no_temp_files, op_same('op_m') AS same_rows, op_pure_units(3) >= 35 AS units_filled, :verified;
SELECT :stats;
DROP INDEX op_idx;

-- the off switch: the heap is read as before 5.1 and the build is refused
SET sift.col_build_source = heap;
\set VERBOSITY terse
:ci;
\set VERBOSITY default
SET sift.col_build_source = auto;
RESET temp_file_limit;

-- forced, with room for the files: no file is written either
SET sift.test_col_open = 1;
:ci;
SELECT current_setting('sift.test_col_spill_bytes')::int = 0 AS no_temp_files, op_same('op_m') AS same_rows, :verified;

-- a merge puts the units in the order of the key and REINDEX builds the index again
SELECT sift.col_merge('op_idx'::regclass, true) IS NOT NULL AS merged;
SELECT op_same('op_m') AS same_rows, op_pure_units(3) >= 35 AS units_after_merge;
REINDEX INDEX op_idx;
SELECT op_same('op_m') AS same_rows, :verified;
DROP INDEX op_idx;

-- more values than the open units hold: the largest is written short, answers stay
SET sift.test_col_open_kb = 40;
SET sift.test_col_open_single = 4;
:ci;
SELECT (regexp_match(current_setting('sift.test_col_open_stats'), 'evictions=(\d+)'))[1]::int AS evictions \gset
SELECT :evictions > 0 AS evicted, op_same('op_m') AS same_rows, :verified;
RESET sift.test_col_open_kb;
RESET sift.test_col_open_single;
DROP INDEX op_idx;

-- mutants: a defect in the open units, each one caught by what it breaks
SET sift.test_col_open_mutant = 1;
:ci;
SELECT op_pure_units(3) < 35 AS value_mixed_with_another, op_same('op_m') AS same_rows;
DROP INDEX op_idx;
SET sift.test_col_open_mutant = 2;
:ci;
SELECT NOT op_same('op_m') AS tail_lost;
DROP INDEX op_idx;
SET sift.test_col_open_mutant = 3;
:ci;
SELECT NOT op_same('op_m') AS unit_written_twice;
DROP INDEX op_idx;
SET sift.test_col_open_kb = 40;
SET sift.test_col_open_single = 4;
SET sift.test_col_open_mutant = 4;
:ci;
SELECT (regexp_match(current_setting('sift.test_col_open_stats'), 'evictions=(\d+)'))[1]::int < :evictions AS cap_not_looked_at, op_same('op_m') AS same_rows;
DROP INDEX op_idx;
RESET sift.test_col_open_kb;
RESET sift.test_col_open_single;
RESET sift.test_col_open_mutant;

DROP TABLE op_m;
DROP FUNCTION op_same(regclass), op_pure_units(int);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
