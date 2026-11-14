-- a report of one list: the parent table is read for the keys of the list, not whole
-- label: `count(DISTINCT parent.id)` with a FILTER of {rows} rows by 2,000 of 100,000 parents, the rows of one list (an EXISTS over a table read through its own index) limiting the keys of the parent table, which is read for them alone (`sift.agg_lookup_cache_kb = 0`, `sift.agg_lookup_shared_kb = 0`)
-- ordered: yes
-- set: sift.agg_lookup_cache_kb = 0; sift.agg_lookup_shared_kb = 0
--@ setup
DROP TABLE IF EXISTS ls_child;
DROP TABLE IF EXISTS ls_parent;
DROP TABLE IF EXISTS ls_list;
CREATE TABLE ls_child (id int, lookup_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ls_child SELECT g, (g * 7) % 100000, g % 11 FROM generate_series(1, {rows}) g;
CREATE TABLE ls_parent (id int PRIMARY KEY, a text) WITH (autovacuum_enabled = false);
INSERT INTO ls_parent SELECT g, CASE WHEN g % 3 = 0 THEN NULL ELSE 'a' || g END FROM generate_series(0, 99999) g;
CREATE TABLE ls_list (list int, lookup_id int, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO ls_list SELECT g % 50, g, NULL FROM generate_series(0, 99999) g;
CREATE INDEX ls_list_k ON ls_list (list, lookup_id) WHERE deleted_at IS NULL;
CREATE INDEX ls_child_col ON ls_child USING sift_col (id, lookup_id, v);
CREATE INDEX ls_parent_col ON ls_parent USING sift_col (id, (nullif(btrim(a), '') IS NOT NULL));
VACUUM (FREEZE, ANALYZE) ls_child, ls_parent, ls_list
--@ pg
SELECT c.v % 3, count(DISTINCT p.id), count(DISTINCT p.id) FILTER (WHERE nullif(btrim(p.a), '') IS NOT NULL) FROM ls_child c JOIN ls_parent p ON p.id = c.lookup_id WHERE EXISTS (SELECT 1 FROM ls_list l WHERE l.deleted_at IS NULL AND l.lookup_id = c.lookup_id AND l.list = 3) GROUP BY 1 ORDER BY 1
