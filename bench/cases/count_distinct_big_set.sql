-- a count(DISTINCT) of a sparse column whose sets hold millions of pairs with a great deal of hash memory: the pairs go through the sort once the hash set outgrows the cache, where the hash set grew to the whole memory
-- label: `count(DISTINCT parent_id)` of {rows} rows by 3,000 groups (the pairs unique but for a few), `work_mem = '4GB'`
-- ordered: yes
-- set: sift.agg_force = on; work_mem = '4GB'
--@ setup
DROP TABLE IF EXISTS cb_child;
CREATE TABLE cb_child (id int, group_id int, parent_id int) WITH (autovacuum_enabled = false);
INSERT INTO cb_child SELECT g, g % 3000, 1 + (g * 7919) % ({rows} * 3) FROM generate_series(1, {rows}) g;
CREATE INDEX cb_child_col ON cb_child USING sift_col (id, group_id, parent_id);
VACUUM (FREEZE, ANALYZE) cb_child
--@ pg
SELECT group_id, count(DISTINCT parent_id) FROM cb_child GROUP BY group_id ORDER BY group_id
