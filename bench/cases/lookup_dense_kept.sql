-- a group by an attribute of a parent that takes few values, run again with the default lookup caches: the table kept as classes of its keys is read from the cache, where the caches loaded it row by row
-- label: `GROUP BY` the level of the parent of {rows} rows (a table of {rows} / 10 keys with 60 distinct levels, a sift_col index of it that holds its key and level), repeated
-- ordered: yes
-- set: sift.agg_force = on
--@ setup
DROP TABLE IF EXISTS dk_parent;
DROP TABLE IF EXISTS dk_child;
CREATE TABLE dk_parent (id int PRIMARY KEY, lvl smallint) WITH (autovacuum_enabled = false);
INSERT INTO dk_parent SELECT g, (g % 60)::smallint FROM generate_series(1, {rows} / 10) g;
CREATE TABLE dk_child (id int, parent_id int) WITH (autovacuum_enabled = false);
INSERT INTO dk_child SELECT g, 1 + (g * 31) % ({rows} / 10) FROM generate_series(1, {rows}) g;
CREATE INDEX dk_parent_col ON dk_parent USING sift_col (id, lvl);
CREATE INDEX dk_child_col ON dk_child USING sift_col (id, parent_id);
VACUUM (FREEZE, ANALYZE) dk_parent, dk_child
--@ pg
SELECT p.lvl, count(*) FROM dk_child c JOIN dk_parent p ON p.id = c.parent_id GROUP BY p.lvl ORDER BY p.lvl
