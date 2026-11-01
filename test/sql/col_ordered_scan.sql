-- An index-only scan of a sift_col index on the first column gives that column in order when the plan reads it alone (ORDER BY, either direction): the order array of the column
-- merged with the write buffer, the heap saying what a row holds when its page is not all-visible. The rows must be the heap's, in order, NULL rows last (first for DESC), after
-- deletes, updates, rows in the write buffer, a transaction's own changes, and for an index whose segments have no order array (the rows are sorted by the scan).
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET timezone = 'UTC';

CREATE TABLE os (id int, k int4, d date, pad text DEFAULT repeat('x', 200));
ALTER TABLE os ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO os (id, k, d) SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 7919) % 3001 - 1500 END, CASE WHEN i % 17 = 0 THEN NULL ELSE date '2020-01-01' + (i * 31) % 700 END
FROM generate_series(1, 9000) i;
CREATE INDEX os_i ON os USING sift_col (k, id) WITH (count_columns = 'off', write_buffer_limit = 256, order_columns = 'k', order_array_columns = 'k', order_layout = 'whole');
CREATE TABLE ot (id int, k int8, pad text DEFAULT repeat('x', 200));
ALTER TABLE ot ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ot (id, k) SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 104729::int8) % 5003 - 2500 END FROM generate_series(1, 6000) i;
CREATE INDEX ot_i ON ot USING sift_col (k, id) WITH (count_columns = 'off', write_buffer_limit = 256, order_layout = 'whole');
VACUUM (ANALYZE) os;
VACUUM (ANALYZE) ot;

-- sorts is 0 for the ordered scan (queries 1 to 4, 6 to 8) and 1 for query 5 (NULLS FIRST ascending is not the order of the scan) and 9 (it reads another column)
-- the rows of q as the plan gives them, in the order it gives them (the ordered scan, or the heap and a Sort with sift off)
CREATE FUNCTION os_run(q text, ios bool, OUT n bigint, OUT md5 text, OUT scans int, OUT sorts int) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('sift.enable_col_scan', ios::text, true);
	PERFORM set_config('enable_seqscan', (NOT ios)::text, true);
	PERFORM set_config('enable_sort', (NOT ios)::text, true);
	PERFORM set_config('enable_bitmapscan', 'false', true);
	scans := 0;
	sorts := 0;
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'Index Only Scan (Backward )?using o._i' THEN scans := scans + 1; END IF;
		IF line ~ '^ *(->  )?Sort$' THEN sorts := sorts + 1; END IF;
	END LOOP;
	EXECUTE 'SELECT count(*), md5(string_agg(coalesce(k::text, ''null''), ''|'')) FROM (' || q || ') w' INTO n, md5;
END $$;

CREATE FUNCTION os_check(q text, OUT rows bigint, OUT agree bool, OUT ordered_scans int, OUT sorts int) LANGUAGE plpgsql AS $$
DECLARE a record; h record;
BEGIN
	SELECT * INTO a FROM os_run(q, true);
	SELECT * INTO h FROM os_run(q, false);
	rows := h.n; agree := a.n = h.n AND a.md5 IS NOT DISTINCT FROM h.md5; ordered_scans := a.scans; sorts := a.sorts;
END $$;

CREATE TABLE os_q (n int, q text);
INSERT INTO os_q VALUES
 (1, 'SELECT k FROM os ORDER BY k'),
 (2, 'SELECT k FROM os ORDER BY k DESC'),
 (3, 'SELECT k FROM os WHERE k BETWEEN -300 AND 700 ORDER BY k'),
 (4, 'SELECT k FROM os WHERE k IS NOT NULL ORDER BY k DESC'),
 (5, 'SELECT k FROM os ORDER BY k NULLS FIRST'),
 (6, 'SELECT k FROM ot ORDER BY k'),
 (7, 'SELECT k FROM ot ORDER BY k DESC'),
 (8, 'SELECT k FROM ot WHERE k < 0 ORDER BY k DESC'),
 (9, 'SELECT k, d FROM os ORDER BY k');
SELECT n, q, (os_check(q)).* FROM os_q ORDER BY n;

-- rows deleted and updated after the build, and rows in the write buffer
DELETE FROM os WHERE id % 10 = 3;
UPDATE os SET k = k + 5000 WHERE id % 10 = 4;
UPDATE os SET k = NULL WHERE id % 10 = 5;
INSERT INTO os (id, k) SELECT 9000 + i, (i * 37) % 4000 - 2000 FROM generate_series(1, 300) i;
DELETE FROM ot WHERE id % 9 = 2;
UPDATE ot SET k = -k WHERE id % 9 = 4;
SELECT n, q, (os_check(q)).* FROM os_q ORDER BY n;

-- a transaction's own changes (the pages are not all-visible, the snapshot sees them), rolled back after
BEGIN;
DELETE FROM os WHERE id % 7 = 1;
UPDATE os SET k = 100000 - id WHERE id % 7 = 2;
INSERT INTO os (id, k) SELECT 20000 + i, i FROM generate_series(1, 50) i;
SELECT n, q, (os_check(q)).* FROM os_q WHERE n <= 5 ORDER BY n;
ROLLBACK;
SELECT n, q, (os_check(q)).* FROM os_q WHERE n <= 5 ORDER BY n;

-- after a vacuum (pages all-visible, the dead entries gone from the heap) and a rebuild of the write buffer
VACUUM os;
VACUUM ot;
SELECT n, q, (os_check(q)).* FROM os_q ORDER BY n;

-- a few rows deleted and vacuumed (the units clear them in place), their addresses taken by new rows with other keys, or the same
DELETE FROM os WHERE id % 500 = 0;
DELETE FROM ot WHERE id % 500 = 0;
VACUUM os;
VACUUM ot;
INSERT INTO os (id, k) SELECT 30000 + i, CASE WHEN i % 2 = 0 THEN (i * 7919) % 3001 - 1500 ELSE 8000 + i END FROM generate_series(1, 1500) i;
INSERT INTO ot (id, k) SELECT 30000 + i, CASE WHEN i % 2 = 0 THEN (i * 104729::int8) % 5003 - 2500 ELSE 9000 + i END FROM generate_series(1, 1000) i;
SELECT n, q, (os_check(q)).* FROM os_q ORDER BY n;

DROP FUNCTION os_check(text), os_run(text, bool);
DROP TABLE os_q, os, ot;
DROP EXTENSION pg_sift CASCADE;
