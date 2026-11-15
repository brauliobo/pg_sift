-- three FILTER aggregates over tests of the columns of a parent table with thousands of tuples of values: the tests are columns of the table, computed once for each of its rows, and the table is kept as classes of its keys by their outcomes
-- label: three `FILTER` aggregates over parent attributes of {rows} rows (a table of {rows} / 10 keys with 7,000 tuples of three columns, a sift_col index of it), repeated
-- ordered: no
-- set: sift.agg_force = on
--@ setup
DROP TABLE IF EXISTS dt_parent;
DROP TABLE IF EXISTS dt_child;
CREATE TABLE dt_parent (id int PRIMARY KEY, a smallint, b smallint, c int) WITH (autovacuum_enabled = false);
INSERT INTO dt_parent SELECT g, g % 7, g % 40, g % 1000 FROM generate_series(1, {rows} / 10) g;
CREATE TABLE dt_child (id int, parent_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO dt_child SELECT g, 1 + (g * 31) % ({rows} / 10), g % 100 FROM generate_series(1, {rows}) g;
CREATE INDEX dt_parent_col ON dt_parent USING sift_col (id, a, b, c);
CREATE INDEX dt_child_col ON dt_child USING sift_col (id, parent_id, v);
VACUUM (FREEZE, ANALYZE) dt_parent, dt_child
--@ pg
SELECT count(*) FILTER (WHERE p.a = 1), count(*) FILTER (WHERE p.b < 5), sum(c.v) FILTER (WHERE p.c = 7) FROM dt_child c JOIN dt_parent p ON p.id = c.parent_id
