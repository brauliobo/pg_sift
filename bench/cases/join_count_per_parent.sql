-- count(x) of a column of a LEFT JOINed child table for each parent, the ten parents with most children: the scan keeps a flag for each child row, not the id of its value
-- label: `count(c.id)` of {rows} child rows (a column with a value in every row) per parent of {rows} / 10, ten rows, a sift_col index of each table, default `work_mem`
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS jc_child;
DROP TABLE IF EXISTS jc_parent;
CREATE TABLE jc_parent (id int PRIMARY KEY, region int) WITH (autovacuum_enabled = false);
INSERT INTO jc_parent SELECT g, g % 50 FROM generate_series(1, {rows} / 10) g;
CREATE TABLE jc_child (id int PRIMARY KEY, parent_id int NOT NULL REFERENCES jc_parent, v int) WITH (autovacuum_enabled = false);
INSERT INTO jc_child SELECT g, 1 + (g::bigint * 7919) % ({rows} / 10), g % 1000 FROM generate_series(1, {rows}) g;
CREATE INDEX jc_parent_col ON jc_parent USING sift_col (id, region);
CREATE INDEX jc_child_col ON jc_child USING sift_col (id, parent_id, v);
VACUUM (FREEZE, ANALYZE) jc_parent, jc_child
--@ pg
SELECT p.id, count(c.id) FROM jc_parent p LEFT JOIN jc_child c ON c.parent_id = p.id GROUP BY p.id ORDER BY 2 DESC, 1 LIMIT 10
