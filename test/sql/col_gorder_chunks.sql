-- The chunks of the global order of a leaf have 512 entries (column format minor 30) and, in a segment written before, 1,024 (minor 21; sift.test_col_gorder_rows = 1024 writes them): a
-- seek, a cursor, a range of addresses and the verify read both, one table holds segments of both, and a merge writes the current ones.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE gc (id int, k int, ts timestamp, pad text DEFAULT repeat('x', 100));
ALTER TABLE gc ALTER COLUMN pad SET STORAGE PLAIN;
SET sift.test_col_gorder_rows = 1024;
INSERT INTO gc (id, k, ts) SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE (i * 7919) % 50021 END, timestamp '2024-01-01' + ((i * 104729::bigint) % 8000000) * interval '1 second' FROM generate_series(1, 20000) i;
CREATE INDEX gc_i ON gc USING sift_col (id, k, ts) WITH (write_buffer_limit = 256, order_columns = 'k, ts', order_array_columns = 'k, ts');
RESET sift.test_col_gorder_rows;
INSERT INTO gc (id, k, ts) SELECT 20000 + i, CASE WHEN i % 89 = 0 THEN NULL ELSE (i * 6841) % 50021 END, timestamp '2024-01-01' + ((i * 7919::bigint) % 8000000) * interval '1 second' FROM generate_series(1, 20000) i;
SELECT sift.col_promote('gc_i'::regclass) >= 0 AS promoted;
VACUUM (ANALYZE) gc;
SELECT sift.col_verify('gc_i', true);
SELECT format, count(*) AS segments FROM sift.col_segments('gc_i') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;

-- the entries of both kinds of segment ascend
SELECT seg, count(*) AS entries, bool_and(key >= prev) FILTER (WHERE NOT null_row) AS ascending
FROM (SELECT seg, pos, key, null_row, lag(key) OVER (PARTITION BY seg ORDER BY pos) AS prev FROM sift._test_col_gorder('gc_i', 1)) e GROUP BY seg ORDER BY seg;

-- the answers through the arrays are the heap's, for ranges that start and end inside a chunk of either size, across chunks, and past the ends
CREATE FUNCTION gc_ids(w text, o text, via_sift boolean) RETURNS bigint[] LANGUAGE plpgsql AS $$
DECLARE r bigint[];
BEGIN
	PERFORM set_config('sift.enable_col_scan', via_sift::text, true), set_config('sift.enable_topn', via_sift::text, true), set_config('enable_seqscan', (NOT via_sift)::text, true),
			set_config('enable_indexscan', via_sift::text, true), set_config('enable_bitmapscan', via_sift::text, true), set_config('enable_indexonlyscan', via_sift::text, true);
	EXECUTE format('SELECT coalesce(array_agg(id), ''{}'') FROM (SELECT id FROM gc WHERE %s %s) z', w, o) INTO r;
	RETURN r;
END $$;
SELECT w, o, gc_ids(w, o, true) = gc_ids(w, o, false) AS same, cardinality(gc_ids(w, o, false)) AS n
FROM (VALUES
	('k BETWEEN 100 AND 300', 'ORDER BY id'),
	('k BETWEEN 100 AND 5000', 'ORDER BY id'),
	('k >= 49000', 'ORDER BY id'),
	('k < 50', 'ORDER BY id'),
	('k = 7919', 'ORDER BY id'),
	('k BETWEEN -5 AND 100000', 'ORDER BY id'),
	('k > 60000', 'ORDER BY id'),
	('ts >= timestamp ''2024-03-01'' AND ts < timestamp ''2024-03-02''', 'ORDER BY id'),
	('k IS NULL', 'ORDER BY id'),
	('k >= 10000', 'ORDER BY k, id LIMIT 10'),
	('k >= 10000', 'ORDER BY k DESC, id LIMIT 10'),
	('k >= 10000', 'ORDER BY k, id LIMIT 10 OFFSET 700'),
	('true', 'ORDER BY k, id LIMIT 300'),
	('true', 'ORDER BY k DESC, id LIMIT 300 OFFSET 1500'),
	('ts >= timestamp ''2024-02-01''', 'ORDER BY ts, id LIMIT 25'),
	('ts >= timestamp ''2024-02-01''', 'ORDER BY ts DESC, id LIMIT 25'),
	('k > 20000', 'ORDER BY k, id LIMIT 2000 OFFSET 11')
) v(w, o);

-- the range reads the addresses of both kinds of segment and no unit
SELECT p.units AS units_with_addresses, p.rows = 0 AS no_row_read, f.units > p.units AS fewer_units
FROM sift._test_col_counters('gc_i', '1:ge:100;1:le:300', 24) p, sift._test_col_counters('gc_i', '1:ge:100;1:le:300', 0) f;

-- a merge writes one segment of the current chunks, and the answers stay the heap's
SELECT sift.col_merge('gc_i'::regclass, true) >= 0 AS merged;
SELECT sift.col_verify('gc_i', true);
SELECT format, count(*) AS segments FROM sift.col_segments('gc_i') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT gc_ids('k BETWEEN 100 AND 5000', 'ORDER BY id', true) = gc_ids('k BETWEEN 100 AND 5000', 'ORDER BY id', false) AS range_same,
	   gc_ids('k >= 10000', 'ORDER BY k DESC, id LIMIT 10', true) = gc_ids('k >= 10000', 'ORDER BY k DESC, id LIMIT 10', false) AS desc_same;
DROP TABLE gc;
DROP FUNCTION gc_ids(text, text, boolean);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
