-- A sift_table whose column segments were written under several layouts (ADD COLUMN between them) is read by one source a layout: each source opens the segments of
-- its own layout and no other, so a scan, and the fetch scan that serves the fetches of an index into one superblock, open every segment once however many
-- layouts there are (a source that opened them all kept what it read of the others until the end of the scan, and a fetch scan until the directory changed).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
-- Format 1: the column segments of a sift_table are Format 1
SET sift.test_create_format1 = on;
CREATE TABLE lo (id int PRIMARY KEY, v int) USING sift_table WITH (autovacuum_enabled = false);
SELECT format('ALTER TABLE lo ADD COLUMN c%s int DEFAULT %s', i, i), format('INSERT INTO lo (id, v) SELECT g, g FROM generate_series(%s, %s) g', i * 1000, i * 1000 + 999),
	'VACUUM lo' FROM generate_series(1, 6) i \gexec
SELECT count(*) AS segments FROM sift.table_segments('lo') WHERE kind = 'columns';
SELECT sift._test_col_segments_opened(true) >= 0 AS reset;
SELECT count(*), sum(c1), sum(c6) FROM lo;
SELECT sift._test_col_segments_opened(true) = (SELECT count(*) FROM sift.table_segments('lo') WHERE kind = 'columns') AS scan_opens_each_once;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT id, v, c1, c6 FROM lo WHERE id IN (3500, 3501, 3502) ORDER BY id;
SELECT sift._test_col_segments_opened(true) = (SELECT count(*) FROM sift.table_segments('lo') WHERE kind = 'columns') AS fetches_open_each_once;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET sift.test_create_format1;
DROP TABLE lo;
DROP EXTENSION pg_sift_test, pg_sift;
