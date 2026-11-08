-- a distinct count by the flags of a lookup table with a row for each of the keys of the scan: the table is kept as classes of its keys, where it was hashed row by row
-- label: `count(DISTINCT parent_id)` of {rows} rows by their lookup and the mask of the parent (a table of {rows} / 3 keys with 40 distinct masks, a sift_col index of it that holds its key and mask), the lookup table loaded by the query (`sift.agg_lookup_cache_kb = 0`, `sift.agg_lookup_shared_kb = 0`)
-- ordered: yes
-- set: sift.agg_force = on; sift.agg_lookup_cache_kb = 0; sift.agg_lookup_shared_kb = 0
--@ setup
DROP TABLE IF EXISTS ld_parent;
DROP TABLE IF EXISTS ld_child;
CREATE TABLE ld_parent (id int PRIMARY KEY, mask smallint) WITH (autovacuum_enabled = false);
INSERT INTO ld_parent SELECT g, (g * 7919 % 40)::smallint FROM generate_series(1, {rows} / 3) g;
CREATE TABLE ld_child (id int, lookup_id int, parent_id int) WITH (autovacuum_enabled = false);
INSERT INTO ld_child SELECT g, g % 200, 1 + (g * 31) % ({rows} / 3) FROM generate_series(1, {rows}) g;
CREATE INDEX ld_parent_col ON ld_parent USING sift_col (id, mask);
CREATE INDEX ld_child_col ON ld_child USING sift_col (id, lookup_id, parent_id);
VACUUM (FREEZE, ANALYZE) ld_parent, ld_child
--@ pg
SELECT c.lookup_id, p.mask, count(DISTINCT c.parent_id) FROM ld_child c JOIN ld_parent p ON p.id = c.parent_id GROUP BY c.lookup_id, p.mask ORDER BY c.lookup_id, p.mask
