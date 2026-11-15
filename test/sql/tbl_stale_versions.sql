-- sift.table_find_stale_versions lists the primary key values that are on more than one row, reading the table by a sequential scan that neither the aggregate
-- engine nor the primary key index can answer. A deferred primary key lets a transaction hold copies of a key, as the old versions of updated rows were after a VACUUM
-- of 4.0.0 to 4.2.1 made them live again.
CREATE EXTENSION pg_sift;
CREATE TABLE sv_sift (id int PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v int) USING sift_table;
CREATE TABLE sv_heap (id int PRIMARY KEY DEFERRABLE INITIALLY DEFERRED, v int);
CREATE TABLE sv_comp (a int, b text, v int, PRIMARY KEY (a, b) DEFERRABLE INITIALLY DEFERRED) USING sift_table;
CREATE TABLE sv_part (id int, v int, PRIMARY KEY (id) DEFERRABLE INITIALLY DEFERRED) PARTITION BY RANGE (id);
CREATE TABLE sv_part_1 PARTITION OF sv_part FOR VALUES FROM (0) TO (1000) USING sift_table;
CREATE TABLE sv_part_2 PARTITION OF sv_part FOR VALUES FROM (1000) TO (2000) USING sift_table;
CREATE TABLE sv_nopk (id int) USING sift_table;
INSERT INTO sv_sift SELECT g, 0 FROM generate_series(1, 3000) g;
INSERT INTO sv_heap SELECT g, 0 FROM generate_series(1, 3000) g;
INSERT INTO sv_comp SELECT g, 'k' || g % 7, 0 FROM generate_series(1, 500) g;
INSERT INTO sv_part SELECT g, 0 FROM generate_series(0, 1999) g;

-- no copies
SELECT * FROM sift.table_find_stale_versions('sv_sift');
SELECT * FROM sift.table_find_stale_versions('sv_heap');
SELECT * FROM sift.table_find_stale_versions('sv_comp');
SELECT * FROM sift.table_find_stale_versions('sv_part');

-- copies, in the transaction that made them; the aggregate engine and the index see none
BEGIN;
INSERT INTO sv_sift SELECT g, 1 FROM generate_series(1, 25) g;
INSERT INTO sv_heap SELECT g, 1 FROM generate_series(1, 25) g;
INSERT INTO sv_comp SELECT g, 'k' || g % 7, 1 FROM generate_series(1, 25) g;
INSERT INTO sv_part SELECT g, 1 FROM generate_series(990, 1010) g;
SELECT * FROM sift.table_find_stale_versions('sv_sift');
SELECT * FROM sift.table_find_stale_versions('sv_sift', 3);
SELECT * FROM sift.table_find_stale_versions('sv_heap');
SELECT * FROM sift.table_find_stale_versions('sv_comp', 4);
SELECT * FROM sift.table_find_stale_versions('sv_part', 2);
SELECT count(*) AS rows_seen, count(DISTINCT id) AS keys_seen FROM sv_sift;
SELECT current_setting('sift.enable_agg') AS enable_agg_after, current_setting('enable_indexscan') AS indexscan_after;
ROLLBACK;

-- a repeatable read snapshot taken before the copies: it finds the copies of its own transaction
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM sv_sift;
INSERT INTO sv_sift SELECT g, 2 FROM generate_series(1, 5) g;
SELECT * FROM sift.table_find_stale_versions('sv_sift');
ROLLBACK;

-- a table without a primary key
\set VERBOSITY terse
SELECT * FROM sift.table_find_stale_versions('sv_nopk');
DROP TABLE sv_sift, sv_heap, sv_comp, sv_part, sv_nopk;
DROP EXTENSION pg_sift;
