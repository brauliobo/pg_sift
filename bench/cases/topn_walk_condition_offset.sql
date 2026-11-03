-- the walk of the order array of a key with a condition on another column (the soft-delete test) asks the condition before it asks whether an entry is exact, so the dead lists of the units it passes are not read
-- label: `WHERE deleted_at IS NULL ORDER BY k, id LIMIT 20 OFFSET rows / 30` after a `DELETE` of 1% of the rows and a `VACUUM`; the variant `btree` is the same rows with a b-tree on (k, id)
-- ordered: yes
--@ setup
CREATE TABLE wco (id int PRIMARY KEY, k int NOT NULL, g int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO wco SELECT i, ((i::bigint * 7919) % {rows})::int + 1, i % 1000, CASE WHEN i % 50 = 0 THEN now() END FROM generate_series(1, {rows}) i;
CREATE INDEX wco_col ON wco USING sift_col (id, k, g, deleted_at);
VACUUM (FREEZE, ANALYZE) wco;
DELETE FROM wco WHERE id % 97 = 1;
VACUUM (ANALYZE) wco;
DROP SCHEMA IF EXISTS c_topn_walk_condition_offset__btree CASCADE;
CREATE SCHEMA c_topn_walk_condition_offset__btree;
CREATE TABLE c_topn_walk_condition_offset__btree.wco AS TABLE wco;
CREATE INDEX wco_bt ON c_topn_walk_condition_offset__btree.wco (k, id);
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) c_topn_walk_condition_offset__btree.wco
--@ pg
SELECT id, k FROM wco WHERE deleted_at IS NULL ORDER BY k, id LIMIT 20 OFFSET {rows} / 30
