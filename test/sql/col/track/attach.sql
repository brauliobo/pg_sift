-- A table whose index tracks its changes becomes a partition, or a child of an inheritance parent, and leaves it again. A statement on the parent fires the statement triggers of the parent
-- and not those of the child, so the index needs triggers on the parent as well (hearing the rows of the children in its transition table): ATTACH and INHERIT make them, DETACH and NO INHERIT
-- remove them, and the log stays complete through all of it, so the reads of the child stay free of the heap, and are the rows of the heap.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
-- Format 1: the tombstone log and the row queue of track_changes are Format 1.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

-- the tables that hold the track_changes triggers, and how many
CREATE VIEW heard AS SELECT tgrelid::regclass AS "table", count(*) AS triggers FROM pg_trigger WHERE tgfoid = 'sift.col_track_old_rows'::regproc GROUP BY tgrelid ORDER BY tgrelid::regclass::text;

-- a partition
CREATE TABLE pa (id int, g int NOT NULL, v int) PARTITION BY RANGE (id);
CREATE TABLE pa1 (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO pa1 SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX pa1_c ON pa1 USING sift_col (id, g, v);
VACUUM pa1;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
SELECT * FROM heard;
ALTER TABLE pa ATTACH PARTITION pa1 FOR VALUES FROM (1) TO (100000);
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
SELECT * FROM heard;
DELETE FROM pa WHERE id % 2 = 0;
UPDATE pa SET id = id + 500 WHERE id % 5 = 1;
UPDATE pa SET v = v + 1 WHERE id % 7 = 0;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
SELECT sift_helpers.groups_differences_sift_vs_heap('pa1') AS groups_differing_attached, sift_helpers.grouped_read_heap_pages('pa1') AS heap_pages_attached;
DELETE FROM pa1 WHERE id % 3 = 0;
SELECT sift_helpers.groups_differences_sift_vs_heap('pa1') AS groups_differing_direct, sift_helpers.grouped_read_heap_pages('pa1') AS heap_pages_direct;
ALTER TABLE pa DETACH PARTITION pa1;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
SELECT * FROM heard;
DELETE FROM pa1 WHERE id % 11 = 0;
UPDATE pa1 SET id = id + 1000 WHERE id % 13 = 0;
SELECT sift_helpers.groups_differences_sift_vs_heap('pa1') AS groups_differing_detached, sift_helpers.grouped_read_heap_pages('pa1') AS heap_pages_detached;
-- and attached again
ALTER TABLE pa ATTACH PARTITION pa1 FOR VALUES FROM (1) TO (100000);
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
DELETE FROM pa WHERE id % 17 = 0;
SELECT sift_helpers.groups_differences_sift_vs_heap('pa1') AS groups_differing_again, sift_helpers.grouped_read_heap_pages('pa1') AS heap_pages_again;
-- DETACH CONCURRENTLY
ALTER TABLE pa DETACH PARTITION pa1 CONCURRENTLY;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa1_c');
SELECT * FROM heard;
-- the index made after the table is a partition already
CREATE TABLE pa2 PARTITION OF pa FOR VALUES FROM (100000) TO (200000);
ALTER TABLE pa2 ADD PRIMARY KEY (id);
INSERT INTO pa2 SELECT 100000 + i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX pa2_c ON pa2 USING sift_col (id, g, v);
VACUUM pa2;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('pa2_c');
SELECT * FROM heard;
DELETE FROM pa WHERE id % 2 = 0;
UPDATE pa SET id = id + 1000 WHERE id % 5 = 1;
SELECT sift_helpers.groups_differences_sift_vs_heap('pa2') AS groups_differing_created, sift_helpers.grouped_read_heap_pages('pa2') AS heap_pages_created;

-- an inheritance child
CREATE TABLE ih (id int, g int NOT NULL, v int);
CREATE TABLE ih1 (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO ih1 SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX ih1_c ON ih1 USING sift_col (id, g, v);
VACUUM ih1;
ALTER TABLE ih1 INHERIT ih;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('ih1_c');
SELECT * FROM heard WHERE "table"::text IN ('ih', 'ih1');
DELETE FROM ih WHERE id % 2 = 0;
UPDATE ih SET id = id + 500 WHERE id % 5 = 1;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('ih1_c');
SELECT sift_helpers.groups_differences_sift_vs_heap('ih1') AS groups_differing_child, sift_helpers.grouped_read_heap_pages('ih1') AS heap_pages_child;
ALTER TABLE ih1 NO INHERIT ih;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('ih1_c');
SELECT * FROM heard WHERE "table"::text IN ('ih', 'ih1');
DELETE FROM ih1 WHERE id % 3 = 0;
SELECT sift_helpers.groups_differences_sift_vs_heap('ih1') AS groups_differing_after_no_inherit, sift_helpers.grouped_read_heap_pages('ih1') AS heap_pages_after_no_inherit;
-- an inheritance parent with an index of its own, and a child with another
CREATE TABLE ih2 (id int PRIMARY KEY, g int NOT NULL, v int) WITH (autovacuum_enabled = false);
INSERT INTO ih2 SELECT i, i % 5, i FROM generate_series(1, 200) i;
CREATE INDEX ih2_c ON ih2 USING sift_col (id, g, v);
VACUUM ih2;
CREATE TABLE ih3 () INHERITS (ih2);
INSERT INTO ih3 SELECT i, i % 5, i FROM generate_series(1001, 1100) i;
CREATE VIEW ih2_only AS SELECT * FROM ONLY ih2;
SELECT track_changes, triggers, complete, heap_free FROM sift.col_tracking('ih2_c');
SELECT * FROM heard WHERE "table"::text IN ('ih2', 'ih3');
DELETE FROM ih2 WHERE id % 2 = 0;
UPDATE ih2 SET id = id + 500 WHERE id % 5 = 1;
SELECT sift_helpers.groups_differences_sift_vs_heap('ih2_only') AS groups_differing_parent, sift_helpers.grouped_read_heap_pages('ih2_only') AS heap_pages_parent;

DROP VIEW ih2_only;
DROP TABLE pa, pa1, ih, ih1, ih2, ih3 CASCADE;
SELECT * FROM heard;
DROP VIEW heard;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
