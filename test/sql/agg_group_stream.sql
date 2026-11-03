-- A scan that gives its groups in the order of the ORDER BY, whose first key is the key of the index units (or a calendar step of it) and so lies in the zones of the
-- superblocks, reads them in the order of the zones and gives the groups below the zone of the next one as they are final: no spill, no hash of partitions and no Sort
-- follow the scan, and a batch is the groups of a stretch of the key. The rows and their order are the normal plan's with the memory of the hash set from 64kB to
-- 4GB, for NULL keys, either direction, rows of the write buffer, updated and deleted rows, and a scan whose zones overlap (it gives up the stream for the spill).
CREATE EXTENSION pg_sift;
SET default_statistics_target = 10000;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '120s';
SET client_min_messages = warning;

-- 60,000 rows a minute apart in the heap order (a superblock is 256 pages: 6,656 rows of 250 bytes), 500 of them without a time, 50 values of k: about 50,000 groups of an hour and k
CREATE TABLE gs (id int, ts timestamp, d date, k int, v int, pad text, f float8) WITH (autovacuum_enabled = false);
ALTER TABLE gs ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO gs
SELECT i, CASE WHEN i % 120 = 0 THEN NULL ELSE timestamp '2025-01-01' + i * interval '1 minute' END, date '2025-01-01' + i / 500, i % 50, i % 1000, repeat('x', 250), (i % 7) * 0.5
FROM generate_series(1, 60000) i;
CREATE INDEX gs_col ON gs USING sift_col (id, ts, d, k, v) WITH (key_cut = 'ts');
-- the same rows in a random heap order: the zones of its superblocks overlap
CREATE TABLE gr (LIKE gs INCLUDING STORAGE) WITH (autovacuum_enabled = false);
INSERT INTO gr SELECT * FROM gs ORDER BY hashint4(id);
CREATE INDEX gr_col ON gr USING sift_col (id, ts, d, k, v);
-- 37,632 rows (three superblocks of 12,544 rows of 100 bytes, as pad is not plain here) a minute apart, then six superblocks whose times lie anywhere in a range of their own (all but one row of each far above the zone's
-- start): the zones of the last ones overlap and nothing is final after the batches of the first, so the scan goes on as a spill does
CREATE TABLE gh (LIKE gs) WITH (autovacuum_enabled = false);
INSERT INTO gh
SELECT i, CASE WHEN i <= 37632 THEN timestamp '2025-01-01' + i * interval '1 minute'
	WHEN (i - 37633) % 12544 = 0 THEN timestamp '2026-01-01' + (ARRAY[0, 20000, 40000, 60000, 80000, 88000])[(i - 37633) / 12544 + 1] * interval '1 hour'
	ELSE timestamp '2026-01-01' + (90000 + (hashint4(i) & 2147483647) % 10000) * interval '1 hour' END,
	date '2025-01-01', (hashint4(i + 1) & 2147483647) % 50, i % 1000, repeat('x', 100), (i % 7) * 0.5
FROM generate_series(1, 112896) i;
CREATE INDEX gh_col ON gh USING sift_col (id, ts, d, k, v);
VACUUM (FREEZE, ANALYZE) gs;
VACUUM (FREEZE, ANALYZE) gr;
VACUUM (FREEZE, ANALYZE) gh;

-- the query with the memory given: is there a Sort above the scan, did the scan give batches or spill, are the rows the normal plan's, in the same order
CREATE FUNCTION stream_case(tbl text, cols text, keys text, ord text, mem text, filt text DEFAULT '', extra text DEFAULT '') RETURNS TABLE (shape text, mem_set text, sorted boolean, streamed boolean, spilled boolean, same boolean)
LANGUAGE plpgsql AS $$
DECLARE q text; l text; srt boolean := false; st boolean := false; sp boolean := false; a text; b text;
BEGIN
	q := format('SELECT %s, count(*) AS c, sum(v) AS s, min(id) AS lo, max(k) AS hi%s FROM %s %s GROUP BY %s ORDER BY %s', cols, extra, tbl, filt, keys, ord);
	PERFORM set_config('work_mem', mem, false), set_config('hash_mem_multiplier', '1', false), set_config('enable_hashagg', 'off', false), set_config('enable_sort', 'off', false);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		srt := srt OR l ~ '^\s*(->\s+)?Sort';
		st := st OR l ~ '[1-9][0-9]* group stream batches';
		sp := sp OR l ~ '[1-9][0-9]* group table spills';
	END LOOP;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'')) FROM (%s) r', q) INTO a;
	RESET work_mem; RESET hash_mem_multiplier; RESET enable_hashagg; RESET enable_sort;
	SET sift.enable_agg = off;
	EXECUTE format('SELECT md5(string_agg(r::text, ''|'')) FROM (%s) r', q) INTO b;
	RESET sift.enable_agg;
	RETURN QUERY SELECT ord, mem, srt, st, sp, a = b;
END $$;

CREATE FUNCTION stream_cases(tbl text) RETURNS TABLE (shape text, mem_set text, sorted boolean, streamed boolean, spilled boolean, same boolean) LANGUAGE sql AS $$
	-- a calendar step of the time, the NULL time last (the default of the ascending order)
	SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h, k', '64kB')
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h, k', '1MB')
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h, k', '4GB')
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h DESC NULLS LAST, k DESC', '64kB')
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''day'', ts) AS h, k', 'date_trunc(''day'', ts), k', 'h DESC NULLS LAST, k', '64kB')
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h, k', '64kB', 'WHERE ts >= timestamp ''2025-02-01''')
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h', 'date_trunc(''hour'', ts)', 'h', '64kB')
	-- a float sum is added in the order of the rows read: the spill
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h, k', '64kB', '', ', sum(f) AS sf, avg(f) AS af')
	-- NULLs first
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h DESC, k', '1MB')
	UNION ALL SELECT * FROM stream_case(tbl, 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h NULLS FIRST, k', '64kB')
	-- the value of a column
	UNION ALL SELECT * FROM stream_case(tbl, 'd, k, v', 'd, k, v', 'd, k, v', '64kB')
	UNION ALL SELECT * FROM stream_case(tbl, 'd, k, v', 'd, k, v', 'd DESC, k, v', '64kB')
	-- the time itself has a value for nearly every row: its groups are the rows, and it is not a key the sweep reads
	UNION ALL SELECT * FROM stream_case(tbl, 'ts, k', 'ts, k', 'ts, k', '64kB')
	UNION ALL SELECT * FROM stream_case(tbl, 'ts, k', 'ts, k', 'ts DESC, k', '1MB')
$$;

SELECT * FROM stream_cases('gs');

-- rows of the write buffer, appended to the heap, with times anywhere in the range (and in none): the superblocks that hold them have no zone to go by and are read first
INSERT INTO gs SELECT 200000 + i, CASE WHEN i % 90 = 0 THEN NULL ELSE timestamp '2025-01-01' + (i * 7919 % 60000) * interval '1 minute' END, date '2025-01-01' + i % 150, i % 50, i % 1000, repeat('y', 100), (i % 7) * 0.5
FROM generate_series(1, 3000) i;
SELECT * FROM stream_cases('gs');

-- updated rows (the old versions are dead, the new ones in the write buffer of every superblock: no superblock has a zone) and deleted ones
UPDATE gs SET ts = ts + interval '3 days' WHERE id % 100 = 0 AND ts IS NOT NULL;
UPDATE gs SET ts = NULL WHERE id % 1777 = 0;
DELETE FROM gs WHERE id % 777 = 0;
SELECT * FROM stream_cases('gs');

-- after the rows are in the units
VACUUM gs;
SELECT * FROM stream_cases('gs');
SELECT sift.col_verify('gs_col', true);

-- the zones of the random heap order overlap: no superblock is final before the end, so the scan keeps the spill
SELECT * FROM stream_case('gr', 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h, k', '64kB');
SELECT * FROM stream_case('gr', 'd, k, v', 'd, k, v', 'd DESC, k, v', '1MB');

-- the batches of the first superblocks, then the spill
SELECT * FROM stream_case('gh', 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h, k', '64kB');
SELECT * FROM stream_case('gh', 'date_trunc(''hour'', ts) AS h, k', 'date_trunc(''hour'', ts), k', 'h DESC NULLS LAST, k', '64kB');

DROP FUNCTION stream_cases(text);
DROP FUNCTION stream_case(text, text, text, text, text, text, text);
DROP TABLE gs;
DROP TABLE gr;
DROP TABLE gh;
DROP EXTENSION pg_sift;
