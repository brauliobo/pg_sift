-- a window over the order of a key (`rank() OVER (ORDER BY k)`) reads the key in order from the index, also under an aggregate, where PostgreSQL scans the table and sorts it for the window; the milliseconds are of whole statements, planning included
-- label: `SELECT max(r) FROM (SELECT rank() OVER (ORDER BY k) r FROM ow) s` over rows rows of a random integer key, `sift_col (k, id)` with its default options; the variant `btree` is the same rows with a b-tree on (k, id)
-- ordered: yes
--@ setup
CREATE TABLE ow (id int NOT NULL, k int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO ow SELECT i, ((i::bigint * 2654435761) % 1000003)::int FROM generate_series(1, {rows}) i;
CREATE INDEX ow_col ON ow USING sift_col (k, id);
VACUUM (FREEZE, ANALYZE) ow;
DROP SCHEMA IF EXISTS c_ordered_window_rank__btree CASCADE;
CREATE SCHEMA c_ordered_window_rank__btree;
CREATE TABLE c_ordered_window_rank__btree.ow (LIKE ow) WITH (autovacuum_enabled = false);
INSERT INTO c_ordered_window_rank__btree.ow SELECT * FROM ow;
CREATE INDEX ow_bt ON c_ordered_window_rank__btree.ow (k, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_ordered_window_rank__btree.ow
--@ pg
SELECT max(r) FROM (SELECT rank() OVER (ORDER BY k) r FROM ow) s
