-- a join of two tables on a key, ordered by the key, merges the ordered reads of the two indexes (no Sort, no table scan) where PostgreSQL scans and sorts both; the milliseconds are of whole statements, planning included
-- label: `SELECT count(*) FROM (SELECT a.k FROM ow a JOIN ob b ON a.k = b.k ORDER BY a.k OFFSET 0) s` over rows rows of a random integer key on each side, `sift_col (k, id)` with its default options; the variant `btree` is the same rows with a b-tree on (k, id)
-- ordered: yes
--@ setup
CREATE TABLE ow (id int NOT NULL, k int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ow SELECT i, ((i::bigint * 2654435761) % 1000003)::int FROM generate_series(1, {rows}) i;
CREATE INDEX ow_col ON ow USING sift_col (k, id);
CREATE TABLE ob (id int NOT NULL, k int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ob SELECT i, ((i::bigint * 104729) % 1000003)::int FROM generate_series(1, {rows}) i;
CREATE INDEX ob_col ON ob USING sift_col (k, id);
VACUUM (FREEZE, ANALYZE) ow, ob;
DROP SCHEMA IF EXISTS c_ordered_join_order__btree CASCADE;
CREATE SCHEMA c_ordered_join_order__btree;
CREATE TABLE c_ordered_join_order__btree.ow (LIKE ow) WITH (autovacuum_enabled = false);
INSERT INTO c_ordered_join_order__btree.ow SELECT * FROM ow;
CREATE INDEX ow_bt ON c_ordered_join_order__btree.ow (k, id);
CREATE TABLE c_ordered_join_order__btree.ob (LIKE ob) WITH (autovacuum_enabled = false);
INSERT INTO c_ordered_join_order__btree.ob SELECT * FROM ob;
CREATE INDEX ob_bt ON c_ordered_join_order__btree.ob (k, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_ordered_join_order__btree.ow, c_ordered_join_order__btree.ob
--@ pg
SELECT count(*) FROM (SELECT a.k FROM ow a JOIN ob b ON a.k = b.k ORDER BY a.k OFFSET 0) s
