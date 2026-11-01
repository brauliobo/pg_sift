-- count(*) of a range of a leaf with a global order, when a few units of the segment are not exact (the heap pages of rows inserted since the last VACUUM, a deleted row): the units that are exact are
-- counted from the entries between the bounds of the range and the others are read, so the rows read are those of the units that are not exact, not the table's. The counts must be the heap's through
-- inserts, deletes, an aborted delete, updates, ties, NULLs and a bigint over its whole range, and a range of more entries than the order hands out as a list (16,384) is counted the same way.
-- A text leaf is counted the same way, from the ranks of its strings between the bounds of its comparisons (ties, the empty string, NULLs, bounds that no string equals or that lie
-- outside the strings, an empty range).
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE d (id int, b int, t int, c bigint, s text, pad text DEFAULT repeat('x', 200));
ALTER TABLE d ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO d (id, b, t, c, s)
SELECT i, CASE WHEN i % 53 = 0 THEN NULL ELSE (i::bigint * 7919 % 100003)::int END, i % 40,
	   CASE i % 5 WHEN 0 THEN 9223372036854775807 WHEN 1 THEN -9223372036854775808 ELSE (i::bigint * 104729 % 1000003 - 500000) * 9000000000000 END,
	   CASE WHEN i % 61 = 0 THEN NULL WHEN i % 47 = 0 THEN '' ELSE chr(97 + (i * 7919 % 26)::int) || (i % 1000)::text END
FROM generate_series(1, 120000) i;
CREATE INDEX d_i ON d USING sift_col (b sift.col_int4_ops, t sift.col_int4_ops, c sift.col_int8_ops, s, id sift.col_int4_ops)
	WITH (write_buffer_limit = 256, order_columns = 'b, t, c', order_array_columns = 'b, t, c, s');
VACUUM (ANALYZE) d;

-- the rows a statement read: 0 when the segments counted
CREATE FUNCTION d_scanned(q text) RETURNS int LANGUAGE plpgsql SET sift.enable_agg = on SET max_parallel_workers_per_gather = 0 SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Rows: (\d+) scanned' THEN RETURN substring(l from 'Rows: (\d+) scanned')::int; END IF;
	END LOOP;
	RETURN -1;
END $$;
CREATE FUNCTION d_agg(w text) RETURNS bigint LANGUAGE plpgsql SET sift.enable_agg = on SET max_parallel_workers_per_gather = 0 SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off AS $$
DECLARE r bigint;
BEGIN EXECUTE 'SELECT count(*) FROM d WHERE ' || w INTO r; RETURN r; END $$;
CREATE FUNCTION d_heap(w text) RETURNS bigint LANGUAGE plpgsql SET sift.enable_agg = off SET sift.enable_col_scan = off SET enable_indexscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint;
BEGIN EXECUTE 'SELECT count(*) FROM d WHERE ' || w INTO r; RETURN r; END $$;

CREATE TABLE d_w (n int, w text);
INSERT INTO d_w VALUES
 (1, 'b BETWEEN 1000 AND 1600'), (2, 'b >= 99500'), (3, 'b < 300'), (4, 'b > 500 AND b <= 1500'), (5, 'b = 4321'), (6, 'b BETWEEN 90 AND 10'), (7, 'b BETWEEN 0 AND 100002'),
 (8, 'b > 1000000'), (9, 't = 7'), (10, 't BETWEEN 3 AND 4'), (11, 'c BETWEEN -2000000000000000 AND 2000000000000000'), (12, 'c = 9223372036854775807'), (13, 'c < -9000000000000000000'),
 (14, 'c >= 9223372036854775800'), (15, 'b IS NOT NULL AND b < 200'), (16, 'b BETWEEN 1000 AND 1600 AND b <> 1100'), (17, 'b < 100 OR b > 99900'),
 (18, 's >= ''c'' AND s < ''f'''), (19, 's > ''m5'' AND s <= ''q'''), (20, 's = ''a12'''), (21, 's < ''b'''), (22, 's >= ''z'''), (23, 's BETWEEN ''d'' AND ''d9'''),
 (24, 's >= '''' AND s < ''a'''), (25, 's > ''zzz'''), (26, 's BETWEEN ''q'' AND ''a'''), (27, 's > ''k'' AND s > ''m'''), (28, 's >= ''e'' AND s < ''e'' '), (29, 's <= ''h100'''), (30, 's = '''''),
 (31, 's COLLATE "C" >= ''c'' AND s COLLATE "C" < ''f''');
SELECT n, left(w, 60) AS w, d_agg(w) = d_heap(w) AS agree, d_heap(w) AS rows, d_scanned('SELECT count(*) FROM d WHERE ' || w) = 0 AS counted FROM d_w ORDER BY n;

-- rows inserted after the VACUUM (the heap pages at the end of the table are not all-visible, the rows are in the write buffer): the range is counted for the units that are exact
INSERT INTO d (id, b, t, c, s) SELECT 120000 + i, 1000 + i % 600, i % 40, i, 'e' || i % 50 FROM generate_series(1, 400) i;
SELECT n, d_agg(w) = d_heap(w) AS agree FROM d_w ORDER BY n;
SELECT d_scanned('SELECT count(*) FROM d WHERE b BETWEEN 1000 AND 1600') < 5000 AS buffer_and_dirty_units_only,
       d_scanned('SELECT count(*) FROM d WHERE b BETWEEN 1000 AND 1600') > 0 AS but_not_nothing,
       d_scanned('SELECT count(*) FROM d WHERE s >= ''c'' AND s < ''f''') BETWEEN 1 AND 4999 AS text_buffer_and_dirty_units_only;
-- a range of most of the rows (far more entries than a list of addresses holds) is counted from the entries too: the units read are the buffer's and the dirty ones
SELECT d_scanned('SELECT count(*) FROM d WHERE b BETWEEN 0 AND 100002') < 5000 AS wide_int_range_counted,
       d_scanned('SELECT count(*) FROM d WHERE s >= ''a'' AND s < ''t''') < 5000 AS wide_text_range_counted,
       d_agg('s >= ''a'' AND s < ''t''') = d_heap('s >= ''a'' AND s < ''t''') AS wide_text_range_agrees;

-- a deleted row in one unit, committed, then one aborted in another
DELETE FROM d WHERE id BETWEEN 100 AND 140;
BEGIN; DELETE FROM d WHERE id BETWEEN 90000 AND 90100; ROLLBACK;
SELECT n, d_agg(w) = d_heap(w) AS agree FROM d_w ORDER BY n;
SELECT d_scanned('SELECT count(*) FROM d WHERE b BETWEEN 1000 AND 1600') < 30000 AS the_exact_units_are_counted;

-- the deleting transaction's own count, an update of the key, a row updated twice (HOT chain)
BEGIN;
DELETE FROM d WHERE b BETWEEN 1000 AND 1100;
SELECT d_agg('b BETWEEN 1000 AND 1600') = d_heap('b BETWEEN 1000 AND 1600') AS own_deletes_agree;
ROLLBACK;
UPDATE d SET b = b + 1, s = 'k' || id WHERE id % 97 = 0;
UPDATE d SET pad = 'y' WHERE id % 89 = 0;
UPDATE d SET pad = 'z' WHERE id % 89 = 0;
SELECT n, d_agg(w) = d_heap(w) AS agree FROM d_w ORDER BY n;

-- dead rows in every unit: nothing is counted from the order, the answers are the same
DELETE FROM d WHERE id % 1000 = 7;
SELECT n, d_agg(w) = d_heap(w) AS agree FROM d_w ORDER BY n;
SELECT d_scanned('SELECT count(*) FROM d WHERE b BETWEEN 1000 AND 1600') > 0 AS dead_rows_are_read;

-- after VACUUM and a promotion
VACUUM d;
SELECT sift.col_promote('d_i') >= 0 AS promoted;
VACUUM d;
SELECT n, d_agg(w) = d_heap(w) AS agree FROM d_w ORDER BY n;

DROP TABLE d, d_w;
DROP FUNCTION d_scanned(text), d_agg(text), d_heap(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
