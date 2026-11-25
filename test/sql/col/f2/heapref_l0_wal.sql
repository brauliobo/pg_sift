-- Heap-reference L0 slabs and the minimal OLTP WAL on format major 2, without shared_preload_libraries: the rows of an INSERT or an UPDATE of a sift_col index over a heap are references to the heap tuples
-- (nothing of the row is written twice), a reader resolves them with its snapshot, a flush with the tuples a vacuum would keep, and the deletes of a statement are appended in place to the deletion vectors.
-- Every query runs through the index (bitmap, no sequential scan) and again over the heap alone; the two must return the same rows.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET sift.test_create_format1 = off;
CREATE EXTENSION pg_walinspect;
SET client_min_messages = warning;
\set VERBOSITY terse

-- What the index wrote since an LSN: the records of the generic resource manager are the index's
CREATE FUNCTION index_bytes(since pg_lsn) RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce(sum(record_length), 0) FROM pg_get_wal_records_info(since, pg_current_wal_insert_lsn()) WHERE resource_manager = 'Generic' $$;

CREATE TABLE h (id int, g int, v int, pad text) WITH (fillfactor = 50);
CREATE INDEX h_idx ON h USING sift_col (id, g, v);
SELECT major FROM sift._test_f2_meta('h_idx');

-- A statement of 2000 rows is references in a slab, not a part: the index wrote a few bytes a row
SELECT pg_current_wal_insert_lsn() AS l0 \gset
INSERT INTO h SELECT i, i % 10, i % 1000, 'x' FROM generate_series(1, 2000) i;
SELECT index_bytes(:'l0') < 2000 * 20 AS under_20_bytes_a_row;
SELECT nparts, total_rows, has_slab FROM sift._test_f2_partitions('h_idx');
SELECT sift._test_f2_slab('h_idx', 1, NULL);
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE g = 3 AND v < 100');
SELECT sift_helpers.f2_same('SELECT id, g, v FROM h WHERE id IN (5, 77, 999, 5000)');

-- One row a transaction, 300 times, as an OLTP client writes: under the 220 bytes a row of the gate
SELECT pg_current_wal_insert_lsn() AS l1 \gset
DO $$ BEGIN FOR i IN 10001..10300 LOOP INSERT INTO h VALUES (i, i % 10, i % 1000, 'one'); COMMIT; END LOOP; END $$;
SELECT index_bytes(:'l1') < 300 * 220 AS under_220_bytes_a_row;
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE pad = ''one'' AND g = 4');

-- A transaction that rolls back, and a savepoint that does: their references are in the slab and their rows are in no result
BEGIN;
INSERT INTO h SELECT i, 1, 1, 'gone' FROM generate_series(20001, 20100) i;
SAVEPOINT a;
INSERT INTO h SELECT i, 2, 2, 'gone' FROM generate_series(20101, 20200) i;
ROLLBACK TO a;
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE pad = ''gone''');
ROLLBACK;
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE pad = ''gone''');
BEGIN;
INSERT INTO h SELECT i, 1, 1, 'kept' FROM generate_series(20001, 20100) i;
SAVEPOINT a;
INSERT INTO h SELECT i, 2, 2, 'kept' FROM generate_series(20101, 20200) i;
RELEASE a;
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE pad = ''kept''');
COMMIT;
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE pad = ''kept''');

-- A flush resolves the references and writes the rows as a part; nothing is lost, nothing is twice
SELECT sift._test_f2_slab_flush('h_idx', 1, NULL);
SELECT nparts >= 1 AS part, has_slab FROM sift._test_f2_partitions('h_idx');
SELECT sift_helpers.f2_same('SELECT id, g, v, pad FROM h WHERE id <= 600 OR pad = ''kept''');
SELECT sift_helpers.f2_same('SELECT g, count(*) FROM h GROUP BY g');
SELECT sift._test_f2_check('h_idx') ~ '^parts=[0-9]+ ' AS consistent;

-- An UPDATE that changes an indexed column writes the new versions as references and the deletes of the old rows; one that does not (a heap-only tuple, the fillfactor leaves the room) writes nothing to the index
SELECT pg_current_wal_insert_lsn() AS l2 \gset
UPDATE h SET v = v + 5000 WHERE id <= 400 AND id % 2 = 0;
SELECT index_bytes(:'l2') < 200 * 120 AS update_under_120_bytes_a_row;
SELECT sift_helpers.f2_same('SELECT id, v FROM h WHERE id <= 400');
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE v >= 5000');
SELECT pg_current_wal_insert_lsn() AS l3 \gset
UPDATE h SET pad = 'hot' WHERE id BETWEEN 500 AND 560;
SELECT index_bytes(:'l3') = 0 AS hot_update_writes_nothing;
SELECT sift_helpers.f2_same('SELECT id, pad FROM h WHERE id BETWEEN 495 AND 565');

-- Deletes of 2% of the rows, twice: in place after the first, a few bytes a row
BEGIN; DELETE FROM h WHERE id % 50 = 0 AND id <= 2000; COMMIT;
SELECT pg_current_wal_insert_lsn() AS l4 \gset
DELETE FROM h WHERE id % 50 = 7 AND id <= 2000;
SELECT index_bytes(:'l4') < 40 * 45 AS delete_under_45_bytes_a_row;
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE id % 50 IN (0, 7) AND id <= 2000');
SELECT sift_helpers.f2_same('SELECT count(*), sum(id) FROM h WHERE g = 3');
BEGIN;
DELETE FROM h WHERE g = 8;
SELECT count(*) FROM h WHERE g = 8;
ROLLBACK;
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE g = 8');

-- VACUUM frees the slots of the dead tuples, and new tuples take them: no reference of the old rows resurrects them
INSERT INTO h SELECT i, i % 10, i % 1000, 'again' FROM generate_series(30001, 30500) i;
DELETE FROM h WHERE pad = 'again' AND id % 2 = 0;
VACUUM h;
INSERT INTO h SELECT i, 3, 3, 'reused' FROM generate_series(31001, 31300) i;
SELECT sift_helpers.f2_same('SELECT id FROM h WHERE pad IN (''again'', ''reused'')');
SELECT sift_helpers.f2_same('SELECT g, count(*), sum(v) FROM h GROUP BY g');
SELECT sift._test_f2_check('h_idx') ~ '^parts=[0-9]+ ' AS consistent;
VACUUM h;
SELECT sift_helpers.f2_same('SELECT id, g, v, pad FROM h WHERE id % 7 = 0');
SELECT sift._test_f2_check('h_idx') ~ '^parts=[0-9]+ ' AS consistent;
DROP TABLE h;
DROP FUNCTION index_bytes(pg_lsn);
DROP EXTENSION pg_walinspect;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
