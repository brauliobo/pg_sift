-- track_changes on partitions: an index of a partition tracks its changes, whether it was made through the partitioned parent or on the partition, and whichever table a statement names (the leaf,
-- the parent or a level between): the leaf has its two statement triggers and so has each table above it. Updates, key changes, row movements between partitions, deletes, MERGE and
-- INSERT ON CONFLICT leave the reads of every partition free of the heap, and equal to the heap's. The triggers follow the index and the partition tree: DROP INDEX, DROP TABLE of a partition, REINDEX,
-- CREATE INDEX CONCURRENTLY, ALTER INDEX .. SET (track_changes), and a tree attached under another.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE VIEW heard AS SELECT tgrelid::regclass AS "table", count(*) AS triggers FROM pg_trigger WHERE tgfoid = 'sift.col_track_old_rows'::regproc GROUP BY tgrelid ORDER BY tgrelid::regclass::text;
-- one row for each partition of t: its index tracks, the log is complete, the SiftAgg read visits no heap page, and the rows are the heap's
CREATE FUNCTION partitions_read(t regclass) RETURNS TABLE (leaf regclass, heap_free bool, heap_pages bigint, differing bigint) LANGUAGE sql AS $$
	SELECT c.oid::regclass, (SELECT k.heap_free FROM sift.col_tracking(i.indexrelid) k), sift_helpers.grouped_read_heap_pages(c.oid), sift_helpers.groups_differences_sift_vs_heap(c.oid)
	FROM pg_partition_tree(t) pt JOIN pg_class c ON c.oid = pt.relid JOIN pg_index i ON i.indrelid = c.oid JOIN pg_class ic ON ic.oid = i.indexrelid AND ic.relam = (SELECT oid FROM pg_am WHERE amname = 'sift_col')
	WHERE pt.isleaf ORDER BY 1::text $$;

-- the index made through the partitioned parent
CREATE TABLE pt (id int PRIMARY KEY, g int NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (1000) WITH (autovacuum_enabled = false);
CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (1000) TO (2000) WITH (autovacuum_enabled = false);
CREATE TABLE pt3 PARTITION OF pt FOR VALUES FROM (2000) TO (3000) WITH (autovacuum_enabled = false);
INSERT INTO pt SELECT i * 3, i % 7, i FROM generate_series(0, 999) i;
CREATE INDEX pt_c ON pt USING sift_col (id, g, v);
VACUUM pt;
SELECT * FROM heard;
SELECT * FROM partitions_read('pt');
-- a statement on the parent: updates, a key change that moves rows to another partition, deletes
UPDATE pt SET v = v + 1 WHERE id % 12 = 0;
UPDATE pt SET id = id + 1001 WHERE id % 30 = 0 AND id < 2000;
DELETE FROM pt WHERE id % 36 = 3;
SELECT * FROM partitions_read('pt');
-- a statement on one partition, and a DML that names only the rows of one
UPDATE pt1 SET v = v + 2 WHERE id % 24 = 0;
DELETE FROM pt2 WHERE id % 60 = 3;
UPDATE pt SET v = v + 3 WHERE id BETWEEN 2000 AND 2200;
SELECT * FROM partitions_read('pt');
-- INSERT ON CONFLICT DO UPDATE, and MERGE with a delete and an update (their reads visit heap pages on a plain table too: the rows they insert and the speculative insertions are not all-visible)
INSERT INTO pt SELECT id, g, v FROM pt WHERE id % 14 = 0 ON CONFLICT (id) DO UPDATE SET v = pt.v + 100;
MERGE INTO pt USING (SELECT i FROM generate_series(0, 2999, 9) i) s ON pt.id = s.i
	WHEN MATCHED AND s.i % 2 = 0 THEN DELETE WHEN MATCHED THEN UPDATE SET v = pt.v + 7;
SELECT leaf, heap_free, differing FROM partitions_read('pt');
-- the same transaction reads its own statement
BEGIN;
DELETE FROM pt WHERE id % 33 = 0;
SELECT sift_helpers.groups_differences_sift_vs_heap('pt1') AS differing_in_the_transaction;
COMMIT;
SELECT * FROM partitions_read('pt');

-- three levels, and a tree attached under another
CREATE TABLE tl (id int PRIMARY KEY, g int NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE tl_a (LIKE tl INCLUDING ALL) PARTITION BY RANGE (id);
CREATE TABLE tl_a0 PARTITION OF tl_a FOR VALUES FROM (0) TO (500) WITH (autovacuum_enabled = false);
CREATE TABLE tl_a1 PARTITION OF tl_a FOR VALUES FROM (500) TO (1000) WITH (autovacuum_enabled = false);
INSERT INTO tl_a SELECT i, i % 5, i FROM generate_series(1, 999) i;
CREATE INDEX tl_a_c ON tl_a USING sift_col (id, g, v);
VACUUM tl_a;
SELECT * FROM heard WHERE "table"::text LIKE 'tl%';
ALTER TABLE tl ATTACH PARTITION tl_a FOR VALUES FROM (0) TO (1000);
SELECT * FROM heard WHERE "table"::text LIKE 'tl%';
UPDATE tl SET v = v + 1 WHERE id % 10 = 0;
UPDATE tl SET g = (g + 1) % 5 WHERE id % 20 = 0;
DELETE FROM tl WHERE id % 25 = 3;
DELETE FROM tl_a WHERE id % 25 = 4;
SELECT * FROM partitions_read('tl');
ALTER TABLE tl DETACH PARTITION tl_a;
SELECT * FROM heard WHERE "table"::text LIKE 'tl%';
DELETE FROM tl_a WHERE id % 25 = 5;
SELECT * FROM partitions_read('tl_a');

-- a partition whose columns are in another order, and with a dropped column: the key is found by its name in the rows of the parent
CREATE TABLE po (id int PRIMARY KEY, g int NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE po1 (dropped int, v int, g int NOT NULL, id int PRIMARY KEY) WITH (autovacuum_enabled = false);
ALTER TABLE po1 DROP COLUMN dropped;
INSERT INTO po1 (id, g, v) SELECT i * 2, i % 5, i FROM generate_series(1, 400) i;
CREATE INDEX po1_c ON po1 USING sift_col (id, g, v);
ALTER TABLE po ATTACH PARTITION po1 FOR VALUES FROM (0) TO (1000);
VACUUM po1;
DELETE FROM po WHERE id % 6 = 0;
UPDATE po SET id = id + 1 WHERE id % 10 = 4;
UPDATE po SET v = v + 1 WHERE id % 7 = 2;
SELECT * FROM partitions_read('po');

-- a row that moves to another partition by an UPDATE that does not assign its key: the entry in the index of the partition it leaves is ended by the trigger of the parent
CREATE TABLE pm (id int NOT NULL, g int NOT NULL, v int) PARTITION BY LIST (g);
CREATE TABLE pm1 PARTITION OF pm FOR VALUES IN (1, 2, 3) WITH (autovacuum_enabled = false);
CREATE TABLE pm2 PARTITION OF pm FOR VALUES IN (4, 5, 6) WITH (autovacuum_enabled = false);
ALTER TABLE pm1 ADD UNIQUE (id);
ALTER TABLE pm2 ADD UNIQUE (id);
INSERT INTO pm SELECT i, 1 + i % 6, i FROM generate_series(1, 1200) i;
CREATE INDEX pm1_c ON pm1 USING sift_col (id, g, v) WITH (track_changes = 'id');
CREATE INDEX pm2_c ON pm2 USING sift_col (id, g, v) WITH (track_changes = 'id');
VACUUM pm;
SELECT * FROM heard WHERE "table"::text LIKE 'pm%';
UPDATE pm SET g = g + 3 WHERE g <= 3 AND id % 4 = 0;
UPDATE pm SET g = g - 3 WHERE g > 3 AND id % 8 = 3;
SELECT * FROM partitions_read('pm');

-- CREATE INDEX CONCURRENTLY on a partition: the triggers are made, and the log is complete after a REINDEX
CREATE TABLE pc (id int PRIMARY KEY, g int NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE pc1 PARTITION OF pc FOR VALUES FROM (0) TO (1000) WITH (autovacuum_enabled = false);
CREATE TABLE pc2 PARTITION OF pc FOR VALUES FROM (1000) TO (10000) WITH (autovacuum_enabled = false);
INSERT INTO pc SELECT i, i % 7, i FROM generate_series(0, 999) i;
CREATE INDEX CONCURRENTLY pc1_c ON pc1 USING sift_col (id, g, v);
SELECT triggers, complete FROM sift.col_tracking('pc1_c');
SELECT * FROM heard WHERE "table"::text LIKE 'pc%';
REINDEX INDEX pc1_c;
VACUUM pc;
SELECT triggers, complete, heap_free FROM sift.col_tracking('pc1_c');
DELETE FROM pc WHERE id % 9 = 0;
UPDATE pc SET id = id + 5000 WHERE id % 10 = 1;
SELECT * FROM partitions_read('pc');
REINDEX INDEX CONCURRENTLY pc1_c;
SELECT triggers, complete FROM sift.col_tracking('pc1_c');
SELECT * FROM heard WHERE "table"::text LIKE 'pc%';

-- the option off and on: the triggers of the parent go and come back
ALTER INDEX pc1_c SET (track_changes = 'off');
SELECT * FROM heard WHERE "table"::text LIKE 'pc%';
ALTER INDEX pc1_c SET (track_changes = 'id');
SELECT triggers, complete, heap_free FROM sift.col_tracking('pc1_c');
SELECT * FROM heard WHERE "table"::text LIKE 'pc%';

-- ctid: the row triggers are on the partition alone, whatever table a statement names
CREATE TABLE pk (id int NOT NULL, g int NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE pk1 PARTITION OF pk FOR VALUES FROM (0) TO (1000) WITH (autovacuum_enabled = false);
INSERT INTO pk SELECT i, i % 7, i FROM generate_series(0, 999) i;
CREATE INDEX pk1_c ON pk1 USING sift_col (id, g, v) WITH (track_changes = 'ctid');
VACUUM pk;
SELECT triggers, complete, heap_free FROM sift.col_tracking('pk1_c');
SELECT * FROM heard WHERE "table"::text LIKE 'pk%';
UPDATE pk SET g = (g + 1) % 7 WHERE id % 5 = 0;
DELETE FROM pk WHERE id % 13 = 0;
SELECT * FROM partitions_read('pk');

-- what goes with the index, the partition and the parent
SELECT * FROM heard;
DROP INDEX pt_c;
SELECT * FROM heard;
CREATE INDEX pt_c ON pt USING sift_col (id, g, v);
SELECT * FROM heard WHERE "table"::text LIKE 'pt%';
DROP TABLE pt2;
SELECT * FROM heard WHERE "table"::text LIKE 'pt%';
DROP INDEX pc1_c;
DROP TABLE pt, tl, tl_a, pc, pk, pm, po CASCADE;
SELECT * FROM heard;
DROP FUNCTION partitions_read(regclass);
DROP VIEW heard;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
