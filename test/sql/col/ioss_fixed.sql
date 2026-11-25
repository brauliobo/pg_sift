-- An index-only scan of a sift_col index whose returned columns are all fixed-size (integers, boolean, date, timestamps, floats) writes the values into one index tuple it keeps; the rows
-- must be the heap's for every width and for the rows with a NULL in a returned column, with the column subsets a plan reads, a rescan, deleted rows and a write buffer.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET timezone = 'UTC';
SET datestyle = 'ISO, YMD';

CREATE TABLE fx (id int, a int2, b int8, f bool, d date, t timestamp, z timestamptz, r float4, x float8, pad text DEFAULT repeat('x', 300));
ALTER TABLE fx ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO fx (id, a, b, f, d, t, z, r, x)
SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE (i * 31) % 20000 - 10000 END, CASE WHEN i % 17 = 0 THEN NULL ELSE i * 1000003::int8 - 5000000000 END,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE i % 3 = 0 END, CASE WHEN i % 19 = 0 THEN NULL ELSE date '2020-01-01' + i % 900 END,
	   CASE WHEN i % 23 = 0 THEN NULL ELSE timestamp '2021-03-04 05:06:07.123456' + i * interval '37 seconds' END,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE timestamptz '2021-03-04 05:06:07+00' + i * interval '91 seconds' END,
	   CASE WHEN i % 7 = 0 THEN NULL ELSE (i % 1000) / 8.0 - 60 END, CASE WHEN i % 5 = 0 THEN NULL ELSE i / 7.0 - 1000 END
FROM generate_series(1, 9000) i;
CREATE INDEX fx_i ON fx USING sift_col (id sift.col_int4_ops, a sift.col_int2_ops, b sift.col_int8_ops, f sift.col_bool_ops, d sift.col_date_ops, t sift.col_timestamp_ops,
	z sift.col_timestamptz_ops, r sift.col_float4_ops, x sift.col_float8_ops) WITH (count_columns = 'off', write_buffer_limit = 256);
VACUUM (ANALYZE) fx;

CREATE FUNCTION fx_mode(ios bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('sift.enable_col_scan', ios::text, true);
	PERFORM set_config('enable_seqscan', (NOT ios)::text, true);
	PERFORM set_config('seq_page_cost', CASE WHEN ios THEN '1000' ELSE '1' END, true);	-- the index-only scan is the plan whatever its price against the heap
	PERFORM set_config('enable_indexscan', 'false', true);
	PERFORM set_config('enable_indexonlyscan', ios::text, true);
	PERFORM set_config('enable_bitmapscan', 'false', true);
END $$;

CREATE FUNCTION fx_run(q text, ios bool, OUT n bigint, OUT md5 text, OUT scans int) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM fx_mode(ios);
	scans := 0;
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'Index Only Scan using fx_i' THEN scans := scans + 1; END IF;
	END LOOP;
	EXECUTE 'SELECT count(*), md5(string_agg(r, ''|'' ORDER BY r)) FROM (SELECT row_to_json(w)::text AS r FROM (' || q || ') w) y' INTO n, md5;
END $$;

CREATE FUNCTION fx_check(q text, OUT rows bigint, OUT agree bool, OUT index_only_scans int) LANGUAGE plpgsql AS $$
DECLARE a record; h record;
BEGIN
	SELECT * INTO a FROM fx_run(q, true);
	SELECT * INTO h FROM fx_run(q, false);
	rows := h.n; agree := a.n = h.n AND a.md5 IS NOT DISTINCT FROM h.md5; index_only_scans := a.scans;
END $$;

CREATE TABLE fx_q (n int, q text);
INSERT INTO fx_q VALUES
 (1, 'SELECT id FROM fx WHERE id BETWEEN 100 AND 8000'),
 (2, 'SELECT a FROM fx WHERE id BETWEEN 100 AND 8000'),
 (3, 'SELECT b FROM fx WHERE id BETWEEN 100 AND 8000'),
 (4, 'SELECT f FROM fx WHERE id BETWEEN 100 AND 8000'),
 (5, 'SELECT d FROM fx WHERE id BETWEEN 100 AND 8000'),
 (6, 'SELECT t FROM fx WHERE id BETWEEN 100 AND 8000'),
 (7, 'SELECT z FROM fx WHERE id BETWEEN 100 AND 8000'),
 (8, 'SELECT r FROM fx WHERE id BETWEEN 100 AND 8000'),
 (9, 'SELECT x FROM fx WHERE id BETWEEN 100 AND 8000'),
 (10, 'SELECT id, a, b, f, d, t, z, r, x FROM fx WHERE id BETWEEN 100 AND 8000'),
 (11, 'SELECT a, f, x FROM fx WHERE id BETWEEN 100 AND 8000'),
 (12, 'SELECT d, r FROM fx WHERE a BETWEEN -500 AND 500'),
 (13, 'SELECT id FROM fx WHERE a BETWEEN -500 AND 500 AND x IS NULL'),
 (14, 'SELECT count(*) FROM (SELECT id FROM fx OFFSET 0) s'),
 (15, 'SELECT a.id, b.x FROM fx a JOIN fx b ON b.id = a.id + 1 WHERE a.id BETWEEN 100 AND 300'),
 (16, 'SELECT id FROM fx WHERE f AND t > timestamp ''2021-03-05''');
SELECT n, left(q, 70) AS q, (fx_check(q)).* FROM fx_q ORDER BY n;

-- rows deleted and updated after the build, and rows still in the write buffer
DELETE FROM fx WHERE id % 10 = 3;
UPDATE fx SET a = a + 1, x = x * 2 WHERE id % 10 = 4;
INSERT INTO fx (id, a, b, f, d, t, z, r, x) SELECT 9000 + i, i, i, i % 2 = 0, NULL, NULL, NULL, NULL, i / 3.0 FROM generate_series(1, 300) i;
SELECT n, left(q, 70) AS q, (fx_check(q)).* FROM fx_q ORDER BY n;

-- the values of a few rows, so that a wrong width or offset shows
SELECT fx_mode(true) IS NOT NULL AS mode;
SELECT id, a, b, f, d, t, z, r, x FROM fx WHERE (id BETWEEN 1 AND 3 OR id BETWEEN 17 AND 20 OR id BETWEEN 29 AND 35 OR id IN (9001, 9300)) ORDER BY id;
SELECT fx_mode(false) IS NOT NULL AS mode;
SELECT id, a, b, f, d, t, z, r, x FROM fx WHERE (id BETWEEN 1 AND 3 OR id BETWEEN 17 AND 20 OR id BETWEEN 29 AND 35 OR id IN (9001, 9300)) ORDER BY id;

DROP FUNCTION fx_check(text), fx_run(text, bool), fx_mode(bool);
DROP TABLE fx_q, fx;
DROP EXTENSION pg_sift CASCADE;
