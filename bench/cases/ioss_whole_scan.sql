-- a whole-index scan of a sift_col index whose returned columns are integers: the values of each row go into one index tuple the scan keeps, not one tuple formed for every row
-- label: `SELECT count(*), sum(k) FROM (SELECT k FROM t OFFSET 0)` (the variant `ioss` forces the index-only scan of the whole index: the aggregate scan off, no sequential, index or bitmap scan) over {rows} rows of 3 integers and a 300-byte string, the index on the three integers; the variant `btree` is the same rows with a b-tree on them
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS iw;
CREATE TABLE iw (id int, k int, a int, pad text);
ALTER TABLE iw ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO iw SELECT g, g % 1000, g % 97, repeat('x', 300) FROM generate_series(1, {rows}) g;
CREATE INDEX iw_col ON iw USING sift_col (id, k, a);
VACUUM (FREEZE, ANALYZE) iw;
DROP SCHEMA IF EXISTS c_ioss_whole_scan__btree CASCADE;
CREATE SCHEMA c_ioss_whole_scan__btree;
CREATE TABLE c_ioss_whole_scan__btree.iw (id int, k int, a int, pad text);
ALTER TABLE c_ioss_whole_scan__btree.iw ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO c_ioss_whole_scan__btree.iw SELECT * FROM iw;
CREATE INDEX iw_bt ON c_ioss_whole_scan__btree.iw (id, k, a);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_ioss_whole_scan__btree.iw
--@ pg
SELECT count(*), sum(k) FROM (SELECT k FROM iw OFFSET 0) s
