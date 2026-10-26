-- VACUUM of a sift_table with deleted rows in units of 71,428 rows: the liveness bits of such a unit (the bits of a unit of the most rows, 74,496, are 9,312 bytes) lie on
-- three pages of the segment, which with the header page are four; clearing them took the header page and two more, and read a page past the end of the table.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
CREATE TABLE lv (id int) USING sift_table;
INSERT INTO lv SELECT generate_series(1, 1000000);
VACUUM (FULL) lv;
SELECT count(*) AS column_segments, sum(units) AS units FROM sift.table_segments('lv') WHERE kind = 'columns';
-- every unit has dead rows over its whole length
DELETE FROM lv WHERE id % 20 = 5;
VACUUM lv;
SELECT count(*) AS live, sum(id) AS sum_id FROM lv;
-- and again, in units whose liveness was cleared once
DELETE FROM lv WHERE id % 20 = 6;
VACUUM (FREEZE) lv;
SELECT count(*) AS live, sum(id) AS sum_id FROM lv;
SELECT count(*) AS live_by_a_scan_of_the_ids FROM lv WHERE id % 20 NOT IN (5, 6);
DROP TABLE lv;
DROP EXTENSION pg_sift;
