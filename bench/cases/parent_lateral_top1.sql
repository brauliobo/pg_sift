-- the latest child of each parent of a kind, a lateral top-1 from the parent table to its children
-- label: the latest child (`LIMIT 1` by date) of each parent of one kind, of {rows} child rows of {rows} / 10 parents, a sift_col index of each table and a btree on the child's parent id
-- ordered: no
--@ setup
DROP TABLE IF EXISTS pcb_child;
DROP TABLE IF EXISTS pcb_parent;
CREATE TABLE pcb_parent (id int PRIMARY KEY, region int, kind int, name text) WITH (autovacuum_enabled = false);
INSERT INTO pcb_parent SELECT g, g % 50, g % 7, 'n' || g FROM generate_series(1, {rows} / 10) g;
CREATE TABLE pcb_child (id int PRIMARY KEY, parent_id int NOT NULL REFERENCES pcb_parent, v int, ts date, flag boolean) WITH (autovacuum_enabled = false);
INSERT INTO pcb_child SELECT g, 1 + (g::bigint * 7919) % ({rows} / 10), g % 1000, date '2026-01-01' + g % 365, g % 5 = 0 FROM generate_series(1, {rows}) g;
CREATE INDEX pcb_parent_col ON pcb_parent USING sift_col (id, region, kind, name);
CREATE INDEX pcb_child_col ON pcb_child USING sift_col (id, parent_id, v, ts, flag);
CREATE INDEX pcb_child_parent ON pcb_child (parent_id);
VACUUM (FREEZE, ANALYZE) pcb_parent, pcb_child
--@ pg
SELECT count(*) FROM pcb_parent p, LATERAL (SELECT c.v FROM pcb_child c WHERE c.parent_id = p.id ORDER BY c.ts DESC, c.id LIMIT 1) l WHERE p.kind = 3
