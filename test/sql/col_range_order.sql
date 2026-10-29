-- A range on a leaf with a global order is a seek in the sorted keys of each segment: the access method hands the heap the addresses of
-- the rows between the bounds, in heap order, with no unit read, and the heap checks the condition again. The answers are the heap's for every type of the array, NULLs, empty,
-- one-sided and whole-column ranges, other conditions, DML, VACUUM (a unit with a dead row is read), several segments, a merge, a segment without the array, and a LIMIT.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE rg (id int, a int2, b int4, c int8, d date, e timestamp, f timestamptz, pad text DEFAULT repeat('x', 200));
ALTER TABLE rg ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO rg (id, a, b, c, d, e, f)
SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE (i * 37) % 3001 - 1500 END, CASE WHEN i % 53 = 0 THEN NULL ELSE (i * 7919) % 100003 END, (i * 104729::bigint) % 1000003 - 500000,
	   date '2020-01-01' + (i * 13) % 2000, timestamp '2020-01-01' + ((i * 7919) % 34560000) * interval '1 second', timestamptz '2020-01-01 00:00:00+00' + ((i * 104729::bigint) % 34560000) * interval '1 second'
FROM generate_series(1, 60000) i;
CREATE INDEX rg_t ON rg USING sift_col (a sift.col_int2_ops, b sift.col_int4_ops, c sift.col_int8_ops, d sift.col_date_ops, e sift.col_timestamp_ops, f sift.col_timestamptz_ops, id sift.col_int4_ops)
	WITH (write_buffer_limit = 256, order_columns = 'a, b, c, d, e, f', order_array_columns = 'a, b, c, d, e, f');
VACUUM (ANALYZE) rg;
SELECT sift.col_verify('rg_t', true);
SELECT count(*) > 0 AS segments, min(format) AS format, sum(units) > 4 AS units FROM sift.col_segments('rg_t') WHERE kind = 'immutable';

-- the answer by an index scan, by a bitmap scan, and of the heap
CREATE FUNCTION rg_idx(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM (SELECT id, pad FROM rg WHERE %s) z', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION rg_bmp(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_indexscan = off SET enable_indexonlyscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM (SELECT id, pad FROM rg WHERE %s) z', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION rg_seq(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM (SELECT id, pad FROM rg WHERE %s) z', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION rg_limit(w text, n int) RETURNS text LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE c bigint; ok bigint;
BEGIN
	EXECUTE format('SELECT count(*), count(*) FILTER (WHERE %s) FROM (SELECT * FROM rg WHERE %s LIMIT %s) z', w, w, n) INTO c, ok;
	RETURN format('%s rows, %s of the condition', c, ok);
END $$;

CREATE TABLE rg_w (n serial, w text);
INSERT INTO rg_w (w) VALUES
 ('b BETWEEN 1000 AND 1200'), ('b BETWEEN 5 AND 1'), ('b > 100 AND b < 100'), ('b < 50'), ('b <= 50'), ('b > 99990'), ('b >= 99990'), ('b = 1000'), ('b BETWEEN 0 AND 100003'),
 ('b > 40000 AND b <= 40300'), ('b IS NULL'), ('b IS NOT NULL AND b < 20'),
 ('a BETWEEN -10 AND 10'), ('a < -1495'), ('a >= 1495'), ('a BETWEEN 32767 AND 32767'),
 ('c BETWEEN -1000 AND 1000'), ('c > 499990'), ('c < -499990'), ('c BETWEEN 9223372036854775800 AND 9223372036854775807'),
 ('d BETWEEN date ''2021-03-01'' AND date ''2021-03-03''') , ('d < date ''2020-01-04'''), ('d >= date ''2025-06-20'''),
 ('e BETWEEN timestamp ''2021-01-01'' AND timestamp ''2021-01-03'''), ('e < timestamp ''2020-01-02'''),
 ('f BETWEEN timestamptz ''2021-01-01+00'' AND timestamptz ''2021-01-03+00'''), ('f >= timestamptz ''2020-12-30+00'''),
 ('b BETWEEN 1000 AND 1200 AND c < 0'), ('b BETWEEN 1000 AND 1400 AND a BETWEEN -100 AND 100'), ('b BETWEEN 1000 AND 1200 OR b > 99990'), ('b BETWEEN 1000 AND 1200 AND id > 30000'),
 ('b BETWEEN 1000 AND 6000'), ('b BETWEEN 1000 AND 20000');
SELECT n, left(w, 60) AS w, rg_idx(w) = rg_seq(w) AS index_scan, rg_bmp(w) = rg_seq(w) AS bitmap_scan, cardinality(rg_seq(w)) AS matches FROM rg_w ORDER BY n;
SELECT n, left(w, 60) AS w, rg_limit(w, 10) AS lim_10, rg_limit(w, 1) AS lim_1 FROM rg_w WHERE n IN (1, 4, 9, 13, 17, 24, 32) ORDER BY n;

-- the range is a seek: the rows listed, no unit read of a segment with no dead row; a range of more rows than the access method sorts (16,384) is read from the units
SELECT p.units AS units_with_addresses, p.rows = 0 AS no_row_read, f.units > p.units AS fewer_units
FROM sift._test_col_counters('rg_t', '1:ge:1000;1:le:1200', 24) p, sift._test_col_counters('rg_t', '1:ge:1000;1:le:1200', 0) f;
SELECT (SELECT count(*) FROM sift._test_col_tids('rg_t', '1:ge:1000;1:le:1200', 24)) = (SELECT count(*) FROM rg WHERE b BETWEEN 1000 AND 1200) AS all_rows_listed;
SELECT p.rows > 0 AS units_read_past_the_limit FROM sift._test_col_counters('rg_t', '1:ge:1000;1:le:20000', 24) p;

-- the planner takes the index for a narrow range, whether the query wants the values of the index or not
CREATE FUNCTION rg_plan(q text) RETURNS text LANGUAGE plpgsql SET sift.enable_col_scan = on SET max_parallel_workers_per_gather = 0 AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '^\s*(->\s*)?(Limit|Index Scan|Index Only Scan|Bitmap Heap Scan|Seq Scan|Custom Scan)' THEN r := r || ' ' || trim(regexp_replace(l, '^\s*->\s*', '')); END IF;
	END LOOP;
	RETURN trim(r);
END $$;
SELECT rg_plan('SELECT id FROM rg WHERE b BETWEEN 1000 AND 1200') AS narrow_cover, rg_plan('SELECT * FROM rg WHERE b BETWEEN 1000 AND 1200') AS narrow_rows,
	   rg_plan('SELECT * FROM rg WHERE b BETWEEN 1000 AND 1200 LIMIT 10') AS narrow_limit;

-- dead rows: the units that hold them are read instead of the addresses of their rows, and the rows of a new key in a reused address are the heap's
DELETE FROM rg WHERE b BETWEEN 1100 AND 1150 AND id < 20000;
DELETE FROM rg WHERE id BETWEEN 100 AND 400;
VACUUM (INDEX_CLEANUP on) rg;
SELECT n, left(w, 60) AS w, rg_idx(w) = rg_seq(w) AS index_scan, rg_bmp(w) = rg_seq(w) AS bitmap_scan, cardinality(rg_seq(w)) AS matches FROM rg_w ORDER BY n;
INSERT INTO rg (id, a, b, c, d, e, f) SELECT i, i % 100, 1000 + i % 300, i, date '2021-03-02', timestamp '2021-01-02', timestamptz '2021-01-02+00' FROM generate_series(60001, 60800) i;
UPDATE rg SET b = 1100, c = 0 WHERE id % 1999 = 0;
SELECT n, left(w, 60) AS w, rg_idx(w) = rg_seq(w) AS index_scan, rg_bmp(w) = rg_seq(w) AS bitmap_scan, cardinality(rg_seq(w)) AS matches FROM rg_w ORDER BY n;
SELECT n, left(w, 60) AS w, rg_limit(w, 10) AS lim_10 FROM rg_w WHERE n IN (1, 4, 17, 24) ORDER BY n;

-- several segments and a merge
SELECT sift.col_promote('rg_t'::regclass) >= 0 AS promoted;
INSERT INTO rg (id, a, b, c, d, e, f) SELECT i, i % 100, (i * 7919) % 100003, i * 3, date '2021-03-02', timestamp '2021-01-02', timestamptz '2021-01-02+00' FROM generate_series(61001, 80000) i;
SELECT sift.col_promote('rg_t'::regclass) >= 0 AS promoted;
SELECT count(*) > 1 AS segments FROM sift.col_segments('rg_t') WHERE kind = 'immutable';
SELECT sift.col_verify('rg_t', true);
SELECT n, left(w, 60) AS w, rg_idx(w) = rg_seq(w) AS index_scan, rg_bmp(w) = rg_seq(w) AS bitmap_scan, cardinality(rg_seq(w)) AS matches FROM rg_w ORDER BY n;
SELECT sift.col_merge('rg_t'::regclass, true) >= 0 AS merged;
SELECT n, left(w, 60) AS w, rg_idx(w) = rg_seq(w) AS index_scan, rg_bmp(w) = rg_seq(w) AS bitmap_scan FROM rg_w ORDER BY n;

-- a segment without the array reads its units, one with it its keys, in one scan
ALTER INDEX rg_t RESET (order_array_columns);
INSERT INTO rg (id, a, b, c, d, e, f) SELECT i, i % 100, (i * 7919) % 100003, i * 3, date '2021-03-02', timestamp '2021-01-02', timestamptz '2021-01-02+00' FROM generate_series(80001, 90000) i;
SELECT sift.col_promote('rg_t'::regclass) >= 0 AS promoted;
SELECT n, left(w, 60) AS w, rg_idx(w) = rg_seq(w) AS index_scan, rg_bmp(w) = rg_seq(w) AS bitmap_scan FROM rg_w ORDER BY n;
DROP TABLE rg;
DROP TABLE rg_w;
DROP FUNCTION rg_idx(text), rg_bmp(text), rg_seq(text), rg_limit(text, int), rg_plan(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
