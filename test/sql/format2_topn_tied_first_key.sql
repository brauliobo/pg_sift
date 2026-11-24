-- ORDER BY k, id LIMIT n over a Format 2.0 index: the first rows of the scan fill the heap, and a granule whose smallest k equals the heap's worst k can still hold a row with a smaller id, so it is
-- read. The rows of the scan are those of the heap, after an INSERT and after a DELETE, ascending and descending.
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tk (id int PRIMARY KEY, k int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO tk SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919) % 5 END, repeat('x', 60) FROM generate_series(1, 12000) i;
CREATE INDEX tk_ix ON tk USING sift_col (id, k) WITH (order_array_columns = 'k');
VACUUM (ANALYZE) tk;

CREATE FUNCTION tk_same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.agg_force = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	EXECUTE 'SELECT string_agg(x::text, '','') FROM (' || q || ') x' INTO a;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE 'SELECT string_agg(x::text, '','') FROM (' || q || ') x' INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

CREATE FUNCTION tk_check() RETURNS TABLE (shape text, same bool) LANGUAGE sql AS $$
	SELECT 'asc 1', tk_same('SELECT id, k FROM tk ORDER BY k, id LIMIT 1')
	UNION ALL SELECT 'asc 3', tk_same('SELECT id, k FROM tk ORDER BY k, id LIMIT 3')
	UNION ALL SELECT 'desc 1', tk_same('SELECT id, k FROM tk ORDER BY k DESC, id LIMIT 1')
	UNION ALL SELECT 'desc id 1', tk_same('SELECT id, k FROM tk ORDER BY k DESC, id DESC LIMIT 1')
$$;

SELECT * FROM tk_check();
INSERT INTO tk SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919) % 300 END, 'y' FROM generate_series(12001, 12500) i;
SELECT * FROM tk_check();
INSERT INTO tk SELECT i, 0, 'z' FROM generate_series(12501, 12600) i;
DELETE FROM tk WHERE id <= 30;
SELECT * FROM tk_check();
DROP TABLE tk CASCADE;
DROP FUNCTION tk_check(), tk_same(text);
DROP EXTENSION pg_sift;
