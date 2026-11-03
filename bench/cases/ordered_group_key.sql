-- a key read in order within a group (`ORDER BY g, k` over the two columns read, no LIMIT) comes from the order of the index, where PostgreSQL scans the table and sorts it; the milliseconds are of whole statements, planning included
-- label: `SELECT count(*) FROM (SELECT g, k FROM ow ORDER BY g, k OFFSET 0) s` over rows rows of 1,000 groups and a random integer key, `sift_col (g, k, id)` with its default options; the variant `btree` is the same rows with a b-tree on (g, k, id)
-- ordered: yes
--@ setup
CREATE TABLE ow (id int NOT NULL, g int NOT NULL, k int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ow SELECT i, (i % 1000), ((i::bigint * 2654435761) % 1000003)::int FROM generate_series(1, {rows}) i;
CREATE INDEX ow_col ON ow USING sift_col (g, k, id);
VACUUM (FREEZE, ANALYZE) ow;
DROP SCHEMA IF EXISTS c_ordered_group_key__btree CASCADE;
CREATE SCHEMA c_ordered_group_key__btree;
CREATE TABLE c_ordered_group_key__btree.ow (LIKE ow) WITH (autovacuum_enabled = false);
INSERT INTO c_ordered_group_key__btree.ow SELECT * FROM ow;
CREATE INDEX ow_bt ON c_ordered_group_key__btree.ow (g, k, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_ordered_group_key__btree.ow
--@ pg
SELECT count(*) FROM (SELECT g, k FROM ow ORDER BY g, k OFFSET 0) s
