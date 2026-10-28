-- count(*) of a range of an integer, date or timestamp leaf with a global order: a segment whose units are all exact (every page
-- all-visible, no dead row) is counted from the entries between the bounds of the range, with no unit read ("Rows Scanned: 0"); the others are read. The counts must be the heap's
-- through DML, VACUUM, the write buffer, promotion and a merge, for every type, and a second test, another aggregate or an OR must not be taken for a range.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE rc (id int, a int2, b int4, c int8, d date, e timestamp, f timestamptz, pad text DEFAULT repeat('x', 200));
ALTER TABLE rc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO rc (id, a, b, c, d, e, f)
SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE (i * 37) % 3001 - 1500 END, CASE WHEN i % 53 = 0 THEN NULL ELSE (i * 7919) % 100003 END, (i * 104729::bigint) % 1000003 - 500000,
	   date '2020-01-01' + (i * 13) % 2000, timestamp '2020-01-01' + ((i * 7919) % 34560000) * interval '1 second', timestamptz '2020-01-01 00:00:00+00' + ((i * 104729::bigint) % 34560000) * interval '1 second'
FROM generate_series(1, 60000) i;
CREATE INDEX rc_i ON rc USING sift_col (a sift.col_int2_ops, b sift.col_int4_ops, c sift.col_int8_ops, d sift.col_date_ops, e sift.col_timestamp_ops, f sift.col_timestamptz_ops, id sift.col_int4_ops)
	WITH (write_buffer_limit = 256, order_columns = 'a, b, c, d, e, f', order_array_columns = 'a, b, c, d, e, f');
VACUUM (ANALYZE) rc;

-- the rows a statement read: 0 when the segments counted
CREATE FUNCTION rc_scanned(q text) RETURNS int LANGUAGE plpgsql SET sift.enable_agg = on SET max_parallel_workers_per_gather = 0 SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET enable_seqscan = off AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Rows: (\d+) scanned' THEN RETURN substring(l from 'Rows: (\d+) scanned')::int; END IF;
	END LOOP;
	RETURN -1;
END $$;
CREATE FUNCTION rc_agg(w text) RETURNS bigint LANGUAGE plpgsql SET sift.enable_agg = on SET max_parallel_workers_per_gather = 0 SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET enable_seqscan = off AS $$
DECLARE r bigint;
BEGIN EXECUTE 'SELECT count(*) FROM rc WHERE ' || w INTO r; RETURN r; END $$;
CREATE FUNCTION rc_heap(w text) RETURNS bigint LANGUAGE plpgsql SET sift.enable_agg = off SET sift.enable_col_scan = off SET enable_indexscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off AS $$
DECLARE r bigint;
BEGIN EXECUTE 'SELECT count(*) FROM rc WHERE ' || w INTO r; RETURN r; END $$;

CREATE TABLE rc_w (n int, w text);
INSERT INTO rc_w VALUES
 (1, 'b BETWEEN 1000 AND 1200'), (2, 'b >= 99000'), (3, 'b < 150'), (4, 'b > 500 AND b <= 5000'), (5, 'b = 4321'), (6, 'b BETWEEN 90 AND 10'),
 (7, 'b BETWEEN 0 AND 100002'), (8, 'b > 1000000'), (9, 'a BETWEEN -50 AND 50'), (10, 'a >= 1499'), (11, 'c BETWEEN -1000 AND 1000'), (12, 'c < -499900'),
 (13, 'd BETWEEN date ''2021-03-01'' AND date ''2021-03-15'''), (14, 'd = date ''2020-05-05'''), (15, 'e >= timestamp ''2020-12-30'' AND e < timestamp ''2021-01-02'''),
 (16, 'f BETWEEN timestamptz ''2020-06-01 00:00:00+00'' AND timestamptz ''2020-06-02 00:00:00+00'''), (17, 'e < timestamp ''2019-01-01'''),
 (18, 'b BETWEEN 1000 AND 1200 AND b <> 1100'), (19, 'b BETWEEN 1000 AND 1200 AND a > 0'), (20, 'b < 100 OR b > 99900'), (21, 'b IS NOT NULL AND b < 100');
SELECT n, left(w, 60) AS w, rc_agg(w) = rc_heap(w) AS agree, rc_heap(w) AS rows, rc_scanned('SELECT count(*) FROM rc WHERE ' || w) = 0 AS counted FROM rc_w ORDER BY n;

-- another aggregate, a group, a second test: the units are read
SELECT rc_scanned('SELECT count(*), sum(id) FROM rc WHERE b BETWEEN 1000 AND 1200') > 0 AS with_a_sum_scans,
       rc_scanned('SELECT b % 2, count(*) FROM rc WHERE b BETWEEN 1000 AND 1200 GROUP BY 1') > 0 AS with_a_group_scans,
       rc_scanned('SELECT count(*) FROM rc WHERE b BETWEEN 1000 AND 1200 AND id > 5') > 0 AS with_another_test_scans;

-- rows in the write buffer, dead rows, rows not yet visible to all: counts stay the heap's, and the segments that cannot be counted are read
INSERT INTO rc (id, a, b, c, d, e, f) SELECT 60000 + i, i % 100, 1000 + i % 300, i, date '2021-03-01' + i % 20, timestamp '2021-01-01', timestamptz '2021-01-01+00' FROM generate_series(1, 150) i;
SELECT n, rc_agg(w) = rc_heap(w) AS agree FROM rc_w WHERE n IN (1, 2, 4, 9, 13) ORDER BY n;
SELECT sift.col_promote('rc_i') >= 0 AS promoted;
SELECT n, rc_agg(w) = rc_heap(w) AS agree FROM rc_w WHERE n IN (1, 2, 4, 9, 13) ORDER BY n;
VACUUM rc;
SELECT rc_scanned('SELECT count(*) FROM rc WHERE b BETWEEN 1000 AND 1200') = 0 AS counted_after_vacuum;
DELETE FROM rc WHERE id % 1000 = 7;
SELECT n, rc_agg(w) = rc_heap(w) AS agree FROM rc_w ORDER BY n;
SELECT rc_scanned('SELECT count(*) FROM rc WHERE b BETWEEN 1000 AND 1200') > 0 AS dead_rows_are_read;
VACUUM rc;
UPDATE rc SET b = b + 1 WHERE id % 97 = 0;
SELECT n, rc_agg(w) = rc_heap(w) AS agree FROM rc_w ORDER BY n;
VACUUM rc;
SELECT sift.col_merge('rc_i', true) >= 0 AS merged;
SELECT n, rc_agg(w) = rc_heap(w) AS agree FROM rc_w ORDER BY n;
SELECT rc_scanned('SELECT count(*) FROM rc WHERE b BETWEEN 1000 AND 1200') = 0 AS counted_after_merge;
SELECT sift.col_verify('rc_i', true);

-- a segment without the array is read
ALTER INDEX rc_i RESET (order_array_columns);
REINDEX INDEX rc_i;
SELECT rc_scanned('SELECT count(*) FROM rc WHERE b BETWEEN 1000 AND 1200') > 0 AS no_array_is_read, rc_agg('b BETWEEN 1000 AND 1200') = rc_heap('b BETWEEN 1000 AND 1200') AS agree;

DROP FUNCTION rc_scanned(text), rc_agg(text), rc_heap(text);
DROP TABLE rc_w, rc;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
