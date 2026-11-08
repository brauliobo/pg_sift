-- distinct counts of a lookup table's key with FILTERs that test its text columns for NULL or blank, which the index of the table holds as booleans: the filters are columns of the lookup table read from the leaves, and the key of the table is the leaf of the scan
-- label: `count(DISTINCT parent.id)` and two `FILTER (WHERE nullif(btrim(col), '') IS [NOT] NULL)` of {rows} rows by 200 lookups (a table of {rows} / 3 keys, a sift_col index of its key and the two tests), the lookup table loaded by the query (`sift.agg_lookup_cache_kb = 0`, `sift.agg_lookup_shared_kb = 0`)
-- ordered: yes
-- set: sift.agg_force = on; sift.agg_lookup_cache_kb = 0; sift.agg_lookup_shared_kb = 0
--@ setup
DROP TABLE IF EXISTS lb_parent;
DROP TABLE IF EXISTS lb_child;
CREATE TABLE lb_parent (id int PRIMARY KEY, a text, b text) WITH (autovacuum_enabled = false);
INSERT INTO lb_parent SELECT g, CASE WHEN g % 3 = 0 THEN NULL ELSE 'name ' || g END, CASE WHEN g % 5 = 0 THEN '  ' ELSE 'city ' || g % 100 END FROM generate_series(1, {rows} / 3) g;
CREATE TABLE lb_child (id int, lookup_id int, parent_id int) WITH (autovacuum_enabled = false);
INSERT INTO lb_child SELECT g, g % 200, 1 + (g * 31) % ({rows} / 3) FROM generate_series(1, {rows}) g;
CREATE INDEX lb_parent_col ON lb_parent USING sift_col (id, (nullif(btrim(a), '') IS NOT NULL), (nullif(btrim(b), '') IS NOT NULL));
CREATE INDEX lb_child_col ON lb_child USING sift_col (id, lookup_id, parent_id);
VACUUM (FREEZE, ANALYZE) lb_parent, lb_child
--@ pg
SELECT c.lookup_id, count(DISTINCT p.id), count(DISTINCT p.id) FILTER (WHERE nullif(btrim(p.a), '') IS NULL), count(DISTINCT p.id) FILTER (WHERE nullif(btrim(p.b), '') IS NOT NULL) FROM lb_child c JOIN lb_parent p ON p.id = c.parent_id GROUP BY 1 ORDER BY 1
