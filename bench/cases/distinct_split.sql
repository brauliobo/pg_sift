-- distinct counts of one parent key under filters of the parent's own columns: one set of ids for the five, not one for each
-- label: `count(DISTINCT parent.id)` and four FILTERed ones by the tests of the parent's text columns, {rows} rows by {rows} / 2 parents, grouped by a column of the rows
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS ds_child;
DROP TABLE IF EXISTS ds_parent;
CREATE TABLE ds_parent (id int PRIMARY KEY, a text, b text, c text, d text) WITH (autovacuum_enabled = false);
INSERT INTO ds_parent SELECT g, CASE WHEN g % 3 = 0 THEN NULL ELSE 'a' || g END, CASE WHEN g % 5 = 0 THEN '  ' ELSE 'b' END,
	CASE WHEN g % 7 = 0 THEN '' ELSE 'c' END, CASE WHEN g % 11 = 0 THEN NULL ELSE 'd' END FROM generate_series(1, {rows} / 2) g;
CREATE TABLE ds_child (id int, group_id int, lookup_id int) WITH (autovacuum_enabled = false);
INSERT INTO ds_child SELECT g, 1 + g % 20, 1 + (g * 7) % ({rows} / 2) FROM generate_series(1, {rows}) g;
CREATE INDEX ds_parent_col ON ds_parent USING sift_col (id, (nullif(btrim(a), '') IS NOT NULL), (nullif(btrim(b), '') IS NOT NULL), (nullif(btrim(c), '') IS NOT NULL), (nullif(btrim(d), '') IS NOT NULL));
CREATE INDEX ds_child_col ON ds_child USING sift_col (id, group_id, lookup_id);
VACUUM (FREEZE, ANALYZE) ds_child, ds_parent
--@ pg
SELECT c.group_id, count(DISTINCT p.id), count(DISTINCT p.id) FILTER (WHERE nullif(btrim(p.a), '') IS NOT NULL), count(DISTINCT p.id) FILTER (WHERE nullif(btrim(p.b), '') IS NOT NULL), count(DISTINCT p.id) FILTER (WHERE nullif(btrim(p.c), '') IS NOT NULL), count(DISTINCT p.id) FILTER (WHERE nullif(btrim(p.d), '') IS NOT NULL) FROM ds_child c JOIN ds_parent p ON p.id = c.lookup_id GROUP BY 1 ORDER BY 1
