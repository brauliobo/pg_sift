-- Format major 2: what an index promises that no query shows at once. The init fork of an unlogged index is written through the bulk writer; a build never takes a row the heap has deleted for a
-- live one; a text predicate is decided by bytes only where the collation says the bytes decide it; a table with more columns than leaves is read within its arrays; an index-only scan returns
-- the values the rows were written with; a granule of one transaction and command keeps nothing in the version section; and the pages of a slab and of the pending deletes carry a generation.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET sift.enable_agg = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

-- The rows of an index-only scan and of the heap alone, and what the scan read of the heap
CREATE FUNCTION ios_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text; plan text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q INTO plan;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_indexscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index <> via_heap THEN 'DIFFERENT: ' || via_index || ' / ' || via_heap
				WHEN plan !~ '"Node Type": "Index Only Scan"' THEN 'no index-only scan'
				ELSE 'same, index-only, heap fetches ' || (regexp_match(plan, '"Heap Fetches": (\d+)'))[1] END;
END $$;

-- The init fork of an unlogged index: ten blocks, written and logged by the bulk writer; the index reads as the heap does
CREATE UNLOGGED TABLE ul (id int, g int, v int, d date);
INSERT INTO ul SELECT i, i % 10, i % 100, date '2020-01-01' + (i % 50) FROM generate_series(1, 20000) i;
CREATE INDEX ul_idx ON ul USING sift_col (id, g, v, d) WITH (key_order = 'id', compression = 'none');
SELECT pg_relation_size('ul_idx', 'init') / current_setting('block_size')::int AS init_blocks;
SELECT sift._test_f2_check('ul_idx') ~ '^parts=1 ' AS consistent;
SELECT f2_same('SELECT id FROM ul WHERE g = 3 AND v < 50');
CREATE UNLOGGED TABLE ul_empty (id int, g int);
CREATE INDEX ul_empty_idx ON ul_empty USING sift_col (id, g) WITH (compression = 'none');
SELECT pg_relation_size('ul_empty_idx', 'init') / current_setting('block_size')::int AS empty_init_blocks;

-- Rows the heap has deleted when the index is built (this transaction's own deletes, and the old versions of its updates) are not live rows: they are deleted in the part, and no snapshot
-- of the transaction, or of the ones after it, sees them
CREATE TABLE d (id int, g int, v int, f float8, b bool, t timestamptz);
INSERT INTO d SELECT i, i % 10, i, i / 7.0, i % 2 = 0, timestamptz '2024-01-01' + i * interval '1 minute' FROM generate_series(1, 30000) i;
BEGIN;
DELETE FROM d WHERE id % 2 = 0;
UPDATE d SET v = v + 1 WHERE id % 3 = 0;
CREATE INDEX d_idx ON d USING sift_col (id, g, v, f, b, t) WITH (key_order = 'id');
SELECT f2_same('SELECT id, v FROM d WHERE g < 5');
SELECT count(*), sum(v) FROM d WHERE g < 5;
COMMIT;
SELECT f2_same('SELECT id, v FROM d WHERE g < 5');
SELECT count(*), sum(v) FROM d WHERE g < 5;
SELECT nrows, ndead, flags & 512 = 0 AS not_sealed FROM sift._test_f2_parts('d_idx', 1, NULL);
SELECT sift._test_f2_check('d_idx') ~ '^parts=1 ' AS consistent;
-- ... and the ones a CREATE INDEX CONCURRENTLY finds deleted and updated stay out
CREATE TABLE dc (id int, g int, v int);
INSERT INTO dc SELECT i, i % 10, i FROM generate_series(1, 30000) i;
DELETE FROM dc WHERE id % 2 = 0;
UPDATE dc SET v = v + 1 WHERE id % 3 = 0;
CREATE INDEX CONCURRENTLY dc_idx ON dc USING sift_col (id, g, v) WITH (key_order = 'id');
SELECT f2_same('SELECT id, v FROM dc WHERE g < 5');
SELECT count(*), sum(v) FROM dc WHERE g < 5;
DELETE FROM dc WHERE id % 5 = 0;
SELECT f2_same('SELECT id, v FROM dc WHERE g < 5');
VACUUM dc;
SELECT f2_same('SELECT id, v FROM dc WHERE g < 5');

-- Text under a collation whose equal strings are not equal bytes, or whose order is not the order of the bytes: the heap decides every row, the index answers as it does
CREATE COLLATION ci_nd (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE TABLE cs (id int, g int, s text COLLATE ci_nd, u text COLLATE "und-x-icu");
INSERT INTO cs SELECT i, i % 5, CASE i % 3 WHEN 0 THEN 'Apple' WHEN 1 THEN 'apple' ELSE 'APPLE' END || (i % 7), CASE i % 4 WHEN 0 THEN 'zeta' ELSE 'Zeta' END || (i % 4) FROM generate_series(1, 20000) i;
CREATE INDEX cs_idx ON cs USING sift_col (id, g, s sift.col_text_ops, u sift.col_text_ops) WITH (key_order = 'g');
SELECT f2_same('SELECT id FROM cs WHERE s = ''apple3''');
SELECT f2_same('SELECT id FROM cs WHERE s IN (''APPLE3'', ''Apple5'') AND g = 1');
SELECT f2_same('SELECT id FROM cs WHERE u = ''Zeta1''');
SELECT f2_same('SELECT id FROM cs WHERE u IN (''zeta0'', ''Zeta2'')');
SELECT count(*) FROM cs WHERE s = 'apple3';
SELECT count(*) FROM cs WHERE s = 'APPLE3' AND g = 1;
SELECT count(*) FROM cs WHERE u = 'Zeta1';

-- A table with more columns than the index has leaves, and a document that is a root array or string that holds the key
CREATE TABLE wd (id int, c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int, c8 int, c9 int, c10 int, c11 int, c12 int, c13 int, c14 int, c15 int, c16 int, c17 int, c18 int, c19 int, c20 int, doc jsonb);
INSERT INTO wd SELECT i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i,
	CASE i % 4 WHEN 0 THEN '["flag_a"]'::jsonb WHEN 1 THEN '{"flag_a": true}' WHEN 2 THEN '"flag_a"' ELSE '{"flag_b": 1}' END FROM generate_series(1, 3000) i;
CREATE INDEX wd_idx ON wd USING sift_col (id, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15, c16, c17, c18, c19, c20, doc sift.col_jsonb_ops)
	WITH (key_order = 'id', paths = 'doc.flag_a bool');
SELECT f2_same('SELECT id FROM wd WHERE doc ? ''flag_a''');
SELECT count(*) FROM wd WHERE doc ? 'flag_a';

-- An index-only scan returns the values the rows were indexed with: integers, floats, booleans, dates, timestamps
CREATE TABLE ix (id int, s2 smallint, g int, big bigint, r real, f float8, b bool, d date, ts timestamp, tz timestamptz, note text);
INSERT INTO ix SELECT i, (i % 1000)::smallint, i % 10, i * 100000000::bigint, (i % 77) / 4.0, i / 7.0 - 1000, i % 3 = 0, date '2020-01-01' + i % 400, timestamp '2021-02-03 04:05:06' + i * interval '1 second',
	timestamptz '2022-03-04 05:06:07+00' + i * interval '1 minute', 'n' || i FROM generate_series(1, 25000) i;
INSERT INTO ix (id, g) VALUES (25001, NULL), (25002, 3);
CREATE INDEX ix_idx ON ix USING sift_col (id, s2, g, big, r, f, b, d, ts, tz) WITH (key_order = 'g');
VACUUM ix;
SELECT ios_same('SELECT id, g FROM ix WHERE g = 3');
SELECT ios_same('SELECT s2, big, r, f, b, d, ts, tz FROM ix WHERE g = 4 AND id < 5000');
SELECT ios_same('SELECT g, count(*), sum(s2) FROM ix WHERE g < 5 GROUP BY g');
SELECT ios_same('SELECT id, g, f FROM ix WHERE g IS NULL');
SELECT ios_same('SELECT count(*) FROM ix WHERE g = 3');
-- ... and the rows the heap has not all-visible are tested in it
UPDATE ix SET s2 = s2 + 1, f = f + 0.5 WHERE id % 50 = 0;
DELETE FROM ix WHERE id % 97 = 0;
SELECT ios_same('SELECT id, s2, f FROM ix WHERE g = 0') ~ '^same, index-only, heap fetches [1-9]' AS reads_the_heap_for_changed_pages;
SELECT regexp_replace(ios_same('SELECT id, s2, f FROM ix WHERE g = 0'), 'heap fetches \d+', 'heap fetches N');

-- A bulk load keeps no version bytes: the granules of one transaction and command are made from their entry; those of more commands keep theirs
CREATE TABLE sx (id int, g int, v int);
CREATE INDEX sx_idx ON sx USING sift_col (id, g, v) WITH (key_order = 'id');
INSERT INTO sx SELECT i, i % 10, i FROM generate_series(1, 20000) i;
SELECT sum(granules) AS granules, sum(single_xact) AS single_xact, sum(vers_bytes) AS vers_bytes FROM sift._test_f2_parts('sx_idx', 1, NULL) p, LATERAL sift._test_f2_vers('sx_idx', 1, NULL, p.part_gen) v;
BEGIN;
INSERT INTO sx SELECT i, i % 10, i FROM generate_series(30001, 30100) i;
INSERT INTO sx SELECT i, i % 10, i FROM generate_series(40001, 40100) i;
COMMIT;
SELECT sum(granules) AS granules, sum(single_xact) AS single_xact, sum(vers_bytes) AS vers_bytes FROM sift._test_f2_parts('sx_idx', 1, NULL) p, LATERAL sift._test_f2_vers('sx_idx', 1, NULL, p.part_gen) v;
SELECT f2_same('SELECT id FROM sx WHERE g = 3 AND id > 29000');
SELECT count(*), sum(v) FROM sx;
BEGIN;
INSERT INTO sx SELECT i, i % 10, i FROM generate_series(50001, 50100) i;
SELECT f2_same('SELECT id FROM sx WHERE id > 50000');
ROLLBACK;
SELECT f2_same('SELECT id FROM sx WHERE id > 50000');

-- The pages of a slab and of the pending deletes of a partition carry a generation, and a reader refuses one that holds none
CREATE TABLE sl (id int, g int, v int);
INSERT INTO sl SELECT i, i % 10, i FROM generate_series(1, 5000) i;
CREATE INDEX sl_idx ON sl USING sift_col (id, g, v) WITH (key_order = 'id', track_changes = 'ctid');
INSERT INTO sl SELECT i, i % 10, i FROM generate_series(5001, 5020) i;
DELETE FROM sl WHERE id IN (3, 5, 7, 5005);
UPDATE sl SET v = v + 1 WHERE id IN (11, 5010);
SELECT f2_same('SELECT id, v FROM sl WHERE g < 3');
SELECT sift._test_f2_check('sl_idx') ~ '^parts=\d+ ' AS consistent;
SELECT bool_or(has_slab) AS has_slab, bool_or(has_pending) AS has_pending FROM sift._test_f2_partitions('sl_idx');
SELECT f2_same('SELECT id, v FROM sl WHERE g >= 3');

DROP TABLE ul, ul_empty, d, dc, cs, wd, ix, sx, sl;
DROP COLLATION ci_nd;
DROP FUNCTION f2_same(text);
DROP FUNCTION ios_same(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
