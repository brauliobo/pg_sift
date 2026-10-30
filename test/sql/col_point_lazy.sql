-- The addresses of a point lookup are decoded as the scan asks for them (SIFT_COLO_POINT_LAZY, an index scan that returns tuples): a cursor over each list the keys
-- name, merged in order, so a scan that stops after a few rows decodes a few. The rows and the addresses are those of the eager decode, in chunks of any size, over
-- lists longer than a read of the cursor, DML, dead rows (those units come after the addresses), several segments, a rescan and a LIMIT.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE lz (id int, k int, g int, h int, pad text DEFAULT repeat('x', 100));
ALTER TABLE lz ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO lz (id, k, g, h) SELECT i, CASE WHEN i % 61 = 0 THEN NULL ELSE i % 7 END, (i * 7919) % 20011, i % 400 FROM generate_series(1, 100000) i;
CREATE INDEX lz_t ON lz USING sift_col (k sift.col_int4_ops, g sift.col_int4_ops, id sift.col_int4_ops, h sift.col_int4_ops) WITH (write_buffer_limit = 256, point_tid_columns = 'k, g, h');
VACUUM (ANALYZE) lz;
SELECT sift.col_verify('lz_t', true);
SELECT count(*) > 0 AS segments, least(min(format), 15) AS format, sum(units) > 4 AS units FROM sift.col_segments('lz_t') WHERE kind = 'immutable';

-- the answer by an index scan that returns tuples (the lazy path), by a bitmap scan, and of the heap
CREATE FUNCTION lz_idx(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM (SELECT id, pad FROM lz WHERE %s) z', w) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION lz_seq(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM (SELECT id, pad FROM lz WHERE %s) z', w) INTO r;
	RETURN r;
END $$;
-- a LIMIT stops the scan early: n rows, every one a row of the condition
CREATE FUNCTION lz_limit(w text, n int) RETURNS text LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE c bigint; ok bigint;
BEGIN
	EXECUTE format('SELECT count(*), count(*) FILTER (WHERE %s) FROM (SELECT * FROM lz WHERE %s LIMIT %s) z', w, w, n) INTO c, ok;
	RETURN format('%s rows, %s of the condition', c, ok);
END $$;
-- the addresses of a lookup in chunks of c (eager when c = 0): the same set, in order, whatever the size of the chunk
CREATE FUNCTION lz_tids(preds text, c int) RETURNS tid[] LANGUAGE sql AS $$
	SELECT coalesce(array_agg(tid ORDER BY tid), '{}') FROM sift._test_col_tids('lz_t', preds, CASE WHEN c = 0 THEN 24 ELSE 56 + (c << 8) END)
$$;

CREATE TABLE lz_q (n serial, preds text, w text);
INSERT INTO lz_q (preds, w) VALUES
 ('0:eq:3', 'k = 3'),
 ('0:eq:99', 'k = 99'),
 ('0:in:1|2', 'k IN (1, 2)'),
 ('0:in:1|2|3|4|5|6|0', 'k IN (0, 1, 2, 3, 4, 5, 6)'),
 ('0:in:0|1|2|3|4|5|6|7|8', 'k IN (0, 1, 2, 3, 4, 5, 6, 7, 8)'),
 ('1:eq:5', 'g = 5'),
 ('1:in:5|6|7|19999', 'g IN (5, 6, 7, 19999)'),
 ('1:in:5|5|5', 'g IN (5, 5, 5)'),
 ('0:eq:3;2:ge:50000', 'k = 3 AND id >= 50000'),
 ('0:eq:3;1:eq:5', 'k = 3 AND g = 5');
-- the lists are read in pieces: 17,000 addresses of one key in 3 or 4 bytes each
SELECT n, preds, lz_tids(preds, 0) = lz_tids(preds, 1) AS chunk_1, lz_tids(preds, 0) = lz_tids(preds, 7) AS chunk_7, lz_tids(preds, 0) = lz_tids(preds, 128) AS chunk_128,
	   lz_tids(preds, 0) = lz_tids(preds, 100000) AS chunk_all, cardinality(lz_tids(preds, 128)) AS addresses
FROM lz_q ORDER BY n;
SELECT n, w, lz_idx(w) = lz_seq(w) AS index_scan, cardinality(lz_seq(w)) AS matches FROM lz_q ORDER BY n;
SELECT n, w, lz_limit(w, 10) AS lim_10, lz_limit(w, 1) AS lim_1, lz_limit(w, 5000) AS lim_5000 FROM lz_q ORDER BY n;

-- the planner takes the index for a LIMIT over a key of many rows (the start is the open and a probe, the heap is read in address order), where it took the heap
CREATE FUNCTION lz_plan(q text) RETURNS text LANGUAGE plpgsql SET sift.enable_col_scan = on SET max_parallel_workers_per_gather = 0 AS $$
DECLARE l text; r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '^\s*(->\s*)?(Limit|Index Scan|Index Only Scan|Bitmap Heap Scan|Seq Scan)' THEN r := r || ' ' || trim(regexp_replace(l, '^\s*->\s*', '')); END IF;
	END LOOP;
	RETURN trim(r);
END $$;
SELECT lz_plan('SELECT * FROM lz WHERE h = 7 LIMIT 10') AS limit_10, lz_plan('SELECT * FROM lz WHERE h = 7') AS all_rows;
SELECT lz_limit('h = 7', 10) AS lim_10, lz_idx('h = 7') = lz_seq('h = 7') AS index_scan;

-- a rescan: the inner side of a nested loop, once for each key
SET sift.enable_col_scan = on;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET max_parallel_workers_per_gather = 0;
SELECT count(*) AS rows, bool_and(z.k = v.x) AS all_of_the_key
FROM generate_series(0, 6) v(x), LATERAL (SELECT k FROM lz WHERE k = v.x LIMIT 3) z;
SELECT count(*) AS rows, sum(z.id) AS ids FROM generate_series(1, 20) v(x), LATERAL (SELECT id FROM lz WHERE g = v.x) z;
RESET sift.enable_col_scan;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET max_parallel_workers_per_gather;

-- a few units with a dead row among the others: the addresses of the others come first, and the units with the dead rows are read after them
DELETE FROM lz WHERE id BETWEEN 1000 AND 1300;
VACUUM (INDEX_CLEANUP on) lz;
SELECT p.units > 0 AS dirty_units_are_read, p.units < f.units AS the_others_are_not, p.rows < f.rows AS fewer_rows
FROM sift._test_col_counters('lz_t', '0:eq:3', 24) p, sift._test_col_counters('lz_t', '0:eq:3', 0) f;
SELECT n, preds, lz_tids(preds, 0) = lz_tids(preds, 1) AS chunk_1, lz_tids(preds, 0) = lz_tids(preds, 128) AS chunk_128 FROM lz_q ORDER BY n;
SELECT n, w, lz_idx(w) = lz_seq(w) AS index_scan, cardinality(lz_seq(w)) AS matches FROM lz_q ORDER BY n;
SELECT n, w, lz_limit(w, 10) AS lim_10, lz_limit(w, 5000) AS lim_5000 FROM lz_q WHERE n IN (1, 3, 4) ORDER BY n;

-- dead rows in every unit: the units that hold them are read instead of the addresses of their rows (which a VACUUM frees to be used again), after the addresses of the others
DELETE FROM lz WHERE k = 3 AND id % 11 = 0;
DELETE FROM lz WHERE g = 5;
SELECT n, preds, lz_tids(preds, 0) = lz_tids(preds, 1) AS chunk_1, lz_tids(preds, 0) = lz_tids(preds, 128) AS chunk_128 FROM lz_q ORDER BY n;
SELECT n, w, lz_idx(w) = lz_seq(w) AS index_scan, cardinality(lz_seq(w)) AS matches FROM lz_q ORDER BY n;
SELECT n, w, lz_limit(w, 10) AS lim_10, lz_limit(w, 5000) AS lim_5000 FROM lz_q ORDER BY n;
VACUUM (INDEX_CLEANUP on) lz;
INSERT INTO lz (id, k, g) SELECT i, i % 7, (i * 7919) % 20011 FROM generate_series(100001, 130000) i;
UPDATE lz SET k = 3, g = 5 WHERE id % 1999 = 0;
SELECT n, w, lz_idx(w) = lz_seq(w) AS index_scan, cardinality(lz_seq(w)) AS matches FROM lz_q ORDER BY n;
SELECT n, w, lz_limit(w, 10) AS lim_10, lz_limit(w, 5000) AS lim_5000 FROM lz_q ORDER BY n;

-- several segments: the lists of the keys come segment by segment
SELECT sift.col_promote('lz_t'::regclass) >= 0 AS promoted;
INSERT INTO lz (id, k, g) SELECT i, i % 7, (i * 7919) % 20011 FROM generate_series(130001, 160000) i;
SELECT sift.col_promote('lz_t'::regclass) >= 0 AS promoted;
SELECT count(*) > 1 AS segments FROM sift.col_segments('lz_t') WHERE kind = 'immutable';
SELECT sift.col_verify('lz_t', true);
SELECT n, preds, lz_tids(preds, 0) = lz_tids(preds, 1) AS chunk_1, lz_tids(preds, 0) = lz_tids(preds, 7) AS chunk_7, lz_tids(preds, 0) = lz_tids(preds, 128) AS chunk_128
FROM lz_q ORDER BY n;
SELECT n, w, lz_idx(w) = lz_seq(w) AS index_scan, cardinality(lz_seq(w)) AS matches FROM lz_q ORDER BY n;
SELECT n, w, lz_limit(w, 10) AS lim_10, lz_limit(w, 5000) AS lim_5000 FROM lz_q ORDER BY n;
SELECT sift.col_merge('lz_t'::regclass, true) >= 0 AS merged;
SELECT n, w, lz_idx(w) = lz_seq(w) AS index_scan FROM lz_q ORDER BY n;
SELECT n, w, lz_limit(w, 10) AS lim_10 FROM lz_q ORDER BY n;

-- the lookup of a key of a row not yet in a segment (the write buffer) comes from the buffer, after the addresses
INSERT INTO lz (id, k, g) SELECT i, 3, 5 FROM generate_series(160001, 160100) i;
SELECT n, w, lz_idx(w) = lz_seq(w) AS index_scan FROM lz_q WHERE n IN (1, 6, 10) ORDER BY n;
SELECT lz_limit('k = 3', 10) AS lim_10;
DROP TABLE lz;
DROP TABLE lz_q;
DROP FUNCTION lz_idx(text), lz_seq(text), lz_limit(text, int), lz_tids(text, int), lz_plan(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
