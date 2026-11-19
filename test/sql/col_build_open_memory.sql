-- The memory of the open units of a build (scope col_build_open of core/membudget.h, sift._test_mem_highwater()): the rows of the values of the key that fill units are held packed as the open
-- units they fill and the units that are complete, until those take the share of the segment writer (a half of the memory of the open path), and are then written. What the scope holds at the end
-- of each row is under what the plan gave it, a quarter of that memory for the open units and a half for the units that wait, whatever the number of rows: 30,000, 35,000 and 40,000 rows of
-- the same table take the same, the units being written as they wait. A build that never writes the units that wait (mutant 6) holds them all and
-- grows with the rows.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.col_build_source = auto;
SET sift.col_unit_rows = 500;
SET sift.test_col_segment_kb = 4000;

CREATE TABLE om_all (id int, g int, k int, note text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO om_all SELECT i, i % 7, CASE WHEN h % 100 < 60 THEN h % 4 ELSE 4 + (h / 100) % 3000 END, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text)
FROM (SELECT i, (hashint4(i)::bigint & 2147483647) AS h FROM generate_series(1, 40000) i) s;
CREATE TABLE om_t30000 AS SELECT * FROM om_all WHERE id <= 30000;
CREATE TABLE om_t35000 AS SELECT * FROM om_all WHERE id <= 35000;
CREATE TABLE om_t40000 AS SELECT * FROM om_all;
VACUUM (FREEZE, ANALYZE) om_t30000;
VACUUM (FREEZE, ANALYZE) om_t35000;
VACUUM (FREEZE, ANALYZE) om_t40000;

CREATE TABLE peaks (n int, peak bigint, mem bigint, unit bigint, units bigint);
CREATE FUNCTION om_peak(n int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE st text;
BEGIN
	PERFORM count(*) FROM sift._test_mem_highwater(true);
	EXECUTE format('CREATE INDEX om_idx ON om_t%s USING sift_col (id, k, g, note, pad) WITH (key_cut = ''k'', point_tid_columns = ''off'', order_array_columns = ''off'')', n);
	st := current_setting('sift.test_col_open_stats');
	INSERT INTO peaks SELECT n, peak, (regexp_match(st, 'mem_kb=(\d+)'))[1]::bigint * 1024, (regexp_match(st, 'pk_kb=(\d+)'))[1]::bigint * 1024, (regexp_match(st, 'units=(\d+)'))[1]::bigint
		FROM sift._test_mem_highwater(false) WHERE scope = 'col_build_open';
	DROP INDEX om_idx;
END $$;
SELECT om_peak(30000);
SELECT om_peak(35000);
SELECT om_peak(40000);

SELECT n, units > 0 AS units_written, peak > 0 AS measured, peak <= mem * 3 / 4 + 2 * unit AS under_the_share FROM peaks ORDER BY n;
SELECT max(peak) < 1.25 * min(peak) AS the_same_at_every_size FROM peaks;

-- the units that wait never written
TRUNCATE peaks;
SET sift.test_col_open_mutant = 6;
SELECT om_peak(30000);
SELECT om_peak(40000);
SELECT bool_or(peak > mem * 3 / 4 + 2 * unit) AS over_the_share FROM peaks;
SELECT (SELECT peak FROM peaks WHERE n = 40000) > 1.2 * (SELECT peak FROM peaks WHERE n = 30000) AS grows_with_the_rows;
RESET sift.test_col_open_mutant;

DROP FUNCTION om_peak(int);
DROP TABLE peaks, om_all, om_t30000, om_t35000, om_t40000;
RESET sift.test_col_segment_kb;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
