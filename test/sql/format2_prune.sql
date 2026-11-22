-- The scan of an index of format major 2 reads only what its predicates can need: the partitions and parts whose key bounds lie outside the key range, and the granules whose
-- zones do, by binary search. The index blocks a point or a range lookup touches do not grow with the table; the rows it returns are exact (the heap needs no second look at the quals).
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
SET max_parallel_maintenance_workers = 0;

-- The blocks the Bitmap Index Scan of a query touches (shared hits and reads of the index)
CREATE FUNCTION idx_blocks(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ' || q INTO j;
	RETURN (SELECT coalesce(sum(("n"->>'Shared Hit Blocks')::int + ("n"->>'Shared Read Blocks')::int), -1)
			FROM jsonb_path_query(j, 'strict $.** ? (@."Node Type" == "Bitmap Index Scan")') n);
END $$;

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

CREATE TABLE small (id int, g int, v int);
INSERT INTO small SELECT i, i % 7, i % 1000 FROM generate_series(1, 30000) i;
CREATE INDEX small_idx ON small USING sift_col (id, g, v) WITH (key_order = 'id');
CREATE TABLE big (id int, g int, v int);
INSERT INTO big SELECT i, i % 7, i % 1000 FROM generate_series(1, 480000) i;
CREATE INDEX big_idx ON big USING sift_col (id, g, v) WITH (key_order = 'id');
SELECT sift._test_f2_check_keys('small_idx') AS small_keys, sift._test_f2_check_keys('big_idx') AS big_keys;
SELECT sift._test_f2_check('big_idx') ~ '^parts=[0-9]+ ' AS consistent;

-- 16 times the rows (and granules) and the same blocks for a point lookup, a short range and an IN list
SELECT idx_blocks('SELECT * FROM big WHERE id = 123456') <= idx_blocks('SELECT * FROM small WHERE id = 12345') + 8 AS point_blocks_flat;
SELECT idx_blocks('SELECT * FROM big WHERE id BETWEEN 400000 AND 400100') <= idx_blocks('SELECT * FROM small WHERE id BETWEEN 20000 AND 20100') + 8 AS range_blocks_flat;
SELECT idx_blocks('SELECT * FROM big WHERE id IN (5, 20000)') <= idx_blocks('SELECT * FROM small WHERE id IN (5, 20000)') + 16 AS in_blocks_flat;
SELECT idx_blocks('SELECT * FROM big WHERE id = 123456') < 40 AS point_blocks_few;
-- a lookup of the whole table reads it all, so the blocks that were pruned above are not an accident of the layout
SELECT idx_blocks('SELECT * FROM big WHERE id > 0') > 10 * idx_blocks('SELECT * FROM big WHERE id = 123456') AS full_reads_more;

-- the answers are those of the heap: equality, ranges at the ends, outside the table, IN, IS NULL, bounds of the type
SELECT f2_same('SELECT * FROM big WHERE id = 123456');
SELECT f2_same('SELECT * FROM big WHERE id BETWEEN 8190 AND 8200');
SELECT f2_same('SELECT * FROM big WHERE id < 3 OR id > 479998');
SELECT f2_same('SELECT * FROM big WHERE id >= 480000');
SELECT f2_same('SELECT * FROM big WHERE id > 480000');
SELECT f2_same('SELECT * FROM big WHERE id <= 0');
SELECT f2_same('SELECT * FROM big WHERE id IN (1, 8192, 8193, 479999, 999999)');
SELECT f2_same('SELECT * FROM big WHERE id = 100 AND g = 2');
SELECT f2_same('SELECT * FROM big WHERE id BETWEEN 100000 AND 100500 AND v < 100');
SELECT f2_same('SELECT * FROM big WHERE id IS NULL');
SELECT f2_same('SELECT * FROM big WHERE id IS NOT NULL AND g = 3 AND v = 77');

-- NULL keys sort after every integer: a granule that mixes both, and a range that ends at the largest integer
INSERT INTO small SELECT NULL, i % 5, i FROM generate_series(1, 20) i;
INSERT INTO small VALUES (2147483647, 1, 1), (-2147483648, 1, 2);
SELECT f2_same('SELECT * FROM small WHERE id IS NULL');
SELECT f2_same('SELECT * FROM small WHERE id >= 2147483647');
SELECT f2_same('SELECT * FROM small WHERE id <= -2147483648');
SELECT f2_same('SELECT * FROM small WHERE id IS NOT NULL AND g = 1 AND v < 3');
VACUUM small;
SELECT sift._test_f2_check_keys('small_idx') ~ ' exact$' AS keys_exact;
SELECT f2_same('SELECT * FROM small WHERE id IS NULL');
SELECT f2_same('SELECT * FROM small WHERE id BETWEEN 29990 AND 30001');

-- compaction keeps the exact range of the rows that survive: the key range of the rewritten part shrinks with the deleted ends
DELETE FROM small WHERE id <= 12000 OR id >= 25000 OR id IS NULL;
VACUUM small;
SELECT sift._test_f2_check_keys('small_idx') ~ ' exact$' AS keys_exact;
SELECT min(key_lo) AS low_end, max(key_hi) AS high_end FROM sift._test_f2_parts('small_idx', 1, NULL);
SELECT f2_same('SELECT * FROM small WHERE id BETWEEN 11990 AND 12010');
SELECT f2_same('SELECT * FROM small WHERE id BETWEEN 24990 AND 25010');

-- an index without a key order has one partition in the order of the TIDs: every part is read for a predicate on another column and the answers are the heap's
CREATE TABLE flat (id int, g int, s text);
INSERT INTO flat SELECT i, i % 11, 'k' || (i % 13) FROM generate_series(1, 40000) i;
CREATE INDEX flat_idx ON flat USING sift_col (id, g, s);
SELECT sift._test_f2_check_keys('flat_idx') ~ ' exact$' AS keys_exact;
SELECT f2_same('SELECT * FROM flat WHERE id = 31337');
SELECT f2_same('SELECT * FROM flat WHERE s = ''k4'' AND g = 2');
SELECT f2_same('SELECT * FROM flat WHERE s LIKE ''k1%'' AND g < 3');

DROP TABLE small, big, flat;
DROP FUNCTION idx_blocks(text), f2_same(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
