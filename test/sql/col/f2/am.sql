-- sift_col on format major 2, the default format: build, CREATE INDEX CONCURRENTLY, INSERT, UPDATE, DELETE, VACUUM, REINDEX and SELECT, with the key order, JSONB paths, NUMERIC, UUID and TEXT columns.
-- Every query runs through the index (bitmap, no sequential scan) and again over the heap alone; the two must return the same rows.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE f2 (id int, g int, v int, price numeric(12, 2), u uuid, s text, doc jsonb);
INSERT INTO f2
SELECT i, i % 10, i % 1000, (i % 777) / 100.0, ('00000000-0000-4000-8000-' || lpad(to_hex(i), 12, '0'))::uuid, 'str' || (i % 50),
	   jsonb_build_object('a', i % 7, 'b', 'k' || (i % 11), 'c', CASE WHEN i % 5 = 0 THEN NULL ELSE i % 3 END)
FROM generate_series(1, 20000) i;
INSERT INTO f2 (id, g) VALUES (20001, NULL), (20002, 3);

-- Format major 2 is requested by the reloption, with a key order, and is the format the index reports
CREATE INDEX f2_idx ON f2 USING sift_col (id, g, v, price, u, s, doc) WITH (key_order = 'g', paths = 'doc.a int, doc.b text, doc.c int');
SELECT major, npartitions >= 1 AS built FROM sift._test_f2_meta('f2_idx');
SELECT sift._test_f2_check('f2_idx') ~ '^parts=[0-9]+ ' AS consistent;

-- Reads: equality, ranges, in-lists, nulls, text, uuid, numeric, jsonb paths, aggregates and ordering
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g = 3 AND v < 100');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE id IN (5, 77, 999, 50000)');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g IS NULL');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE s = ''str7'' AND v BETWEEN 10 AND 400');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE price > 7.5 AND g = 2');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE u = ''00000000-0000-4000-8000-000000000a0b''');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE doc->>''b'' = ''k3'' AND g = 1');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE (doc->>''c'')::int = 2 AND v < 50');
SELECT sift_helpers.f2_same('SELECT g, count(*), sum(v) FROM f2 WHERE v < 300 GROUP BY g');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g = 4 ORDER BY id LIMIT 5');
SELECT count(*), sum(id) FROM f2 WHERE g = 3 AND v < 100;

-- Writes: INSERT, UPDATE and DELETE are visible to the index at once and through the staged rows before any flush
INSERT INTO f2 SELECT i, i % 10, i % 1000, 1.5, gen_random_uuid(), 'new', '{"a": 1}' FROM generate_series(30001, 31000) i;
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE s = ''new'' AND g = 5');
UPDATE f2 SET v = 5000, s = 'moved' WHERE g = 3 AND v < 100;
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE s = ''moved''');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g = 3 AND v < 100');
DELETE FROM f2 WHERE g = 7;
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g = 7');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE v < 10');
BEGIN;
DELETE FROM f2 WHERE g = 8;
SELECT count(*) FROM f2 WHERE g = 8;
ROLLBACK;
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g = 8');

-- VACUUM removes the dead rows from the index (deletion vectors) and compacts the partitions
VACUUM f2;
SELECT sift._test_f2_check('f2_idx') ~ '^parts=[0-9]+ ' AS consistent;
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g IN (3, 7, 8) AND v < 600');
DELETE FROM f2 WHERE id % 2 = 0;
VACUUM f2;
SELECT sift._test_f2_check('f2_idx') ~ '^parts=[0-9]+ ' AS consistent;
SELECT sift_helpers.f2_same('SELECT g, count(*) FROM f2 GROUP BY g');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE s = ''str9''');

-- REINDEX rebuilds the index with the same options; CREATE INDEX CONCURRENTLY builds the same index beside a live table
REINDEX INDEX f2_idx;
SELECT sift._test_f2_check('f2_idx') ~ '^parts=[0-9]+ ' AS consistent;
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g = 1 AND v BETWEEN 100 AND 200');
DROP INDEX f2_idx;
CREATE INDEX CONCURRENTLY f2_cic ON f2 USING sift_col (id, g, v, s, doc) WITH (paths = 'doc.a int, doc.b text');
SELECT sift._test_f2_check('f2_cic') ~ '^parts=[0-9]+ ' AS consistent;
SELECT indisvalid FROM pg_index WHERE indexrelid = 'f2_cic'::regclass;
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE g = 1 AND v BETWEEN 100 AND 200');
INSERT INTO f2 (id, g, v, s) VALUES (40001, 1, 150, 'after');
SELECT sift_helpers.f2_same('SELECT id FROM f2 WHERE s = ''after''');

-- An empty table, and a table whose rows all go
CREATE TABLE f2_empty (id int, g int);
CREATE INDEX f2_empty_idx ON f2_empty USING sift_col (id, g);
SELECT sift_helpers.f2_same('SELECT id FROM f2_empty WHERE g = 1');
INSERT INTO f2_empty SELECT i, i % 3 FROM generate_series(1, 100) i;
DELETE FROM f2_empty;
VACUUM f2_empty;
SELECT sift_helpers.f2_same('SELECT id FROM f2_empty WHERE g = 1');
SELECT sift._test_f2_check('f2_empty_idx') ~ '^parts=[0-9]+ ' AS consistent;

DROP TABLE f2, f2_empty;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
