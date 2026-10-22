-- A row of a sift_table that does not fit one write buffer (4 MB by default, 8 MB at most) used to be refused. With sift.table_wide_rows = on it is
-- written to tail pages that the page table of the buffer does not list, up to PostgreSQL's 1 GB; the buffer that takes one is format 11, the
-- others keep the layout of earlier versions. The rows survive an update, a delete, a rollback and the promotion into columns.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE wr (id int, g int, v text) USING sift_table;
INSERT INTO wr SELECT i, i % 3, repeat(md5(i::text), 100) FROM generate_series(1, 50) i;

-- off (the default): the row is refused, and the hint names the setting
INSERT INTO wr VALUES (100, 0, repeat('a', 5000000));
SELECT count(*) AS rows_after_refusal FROM wr;

SET sift.table_wide_rows = on;
-- 5 MB (beyond the default buffer) and 12 MB (beyond the largest one) next to ordinary rows
INSERT INTO wr VALUES (100, 0, repeat('a', 5000000)), (101, 1, repeat('bc', 6000000)), (102, 2, 'short');
SELECT id, length(v), md5(v) FROM wr WHERE id >= 100 ORDER BY id;
SELECT count(*), sum(length(v)) FROM wr;
SELECT count(*) AS wrong FROM wr WHERE id <= 50 AND v <> repeat(md5(id::text), 100);

-- only the buffer that took them is marked: kind, format
SELECT kind, format FROM sift.table_segments('wr') WHERE kind IN ('buffer', 'sealed buffer') ORDER BY gen;

-- update and delete keep the other rows, in the buffer and after the promotion into columns
UPDATE wr SET v = repeat('u', 7000000) WHERE id = 100;
DELETE FROM wr WHERE id = 101;
SELECT id, length(v), left(v, 3) FROM wr WHERE id >= 100 ORDER BY id;
VACUUM wr;
SELECT id, length(v), left(v, 3), md5(v) FROM wr WHERE id >= 100 ORDER BY id;
SELECT count(*), sum(length(v)) FROM wr;
SELECT count(*) AS wrong FROM wr WHERE id <= 50 AND v <> repeat(md5(id::text), 100);
SELECT kind, format FROM sift.table_segments('wr') WHERE kind IN ('buffer', 'sealed buffer') ORDER BY gen;
SELECT sift._test_tbl_verify('wr') AS verified_rows;

-- the scan and the normal plan read the same rows
SELECT g, count(*), sum(length(v)), min(length(v)), max(length(v)) FROM wr GROUP BY g ORDER BY g;
SET sift.enable_agg = off;
SELECT g, count(*), sum(length(v)), min(length(v)), max(length(v)) FROM wr GROUP BY g ORDER BY g;
RESET sift.enable_agg;

-- a rolled back wide row is not visible, and VACUUM returns its pages: the file does not keep them
BEGIN;
INSERT INTO wr VALUES (200, 0, repeat('r', 6000000));
SELECT length(v) FROM wr WHERE id = 200;
ROLLBACK;
SELECT count(*) AS rolled_back FROM wr WHERE id = 200;
VACUUM wr;
SELECT count(*), sum(length(v)) FROM wr;
SELECT relation_blocks < 2000 AS file_small FROM sift.table_space('wr');

-- a wide row in a transaction that commits, read in the same transaction after an update
BEGIN;
INSERT INTO wr VALUES (300, 1, repeat('c', 6000000));
UPDATE wr SET v = v || 'd' WHERE id = 300;
SELECT length(v), right(v, 2) FROM wr WHERE id = 300;
COMMIT;
SELECT length(v), right(v, 2) FROM wr WHERE id = 300;

-- a row that fits keeps the earlier layout, whatever the setting says
CREATE TABLE wn (id int, v text) USING sift_table;
INSERT INTO wn SELECT i, repeat('n', 100000) FROM generate_series(1, 20) i;
SELECT kind, format FROM sift.table_segments('wn') WHERE kind IN ('buffer', 'sealed buffer') ORDER BY gen;
SELECT count(*), sum(length(v)) FROM wn;

-- the continuation page of a row comes from disk: one that is not, or holds no bytes, is corruption
SELECT sift._test_tbl_ovf_piece(100, 256) AS piece;
SELECT sift._test_tbl_ovf_piece(0, 256);
SELECT sift._test_tbl_ovf_piece(-1, 256);
SELECT sift._test_tbl_ovf_piece(100, 0);
SELECT sift._test_tbl_ovf_piece(100000, 256);

DROP TABLE wr, wn;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
