-- an anti join by a bigint key of a set of 1,000 members: the byte table over the keys answers it for every row, where a bigint key took the hash probe of the lookup join
-- label: `NOT EXISTS` of a set of 1,000 members by the bigint `parent_id` of {rows} rows (a sift_col index of the rows, a b-tree of the set)
-- ordered: no
--@ setup
DROP TABLE IF EXISTS sl_fact;
DROP TABLE IF EXISTS sl_members;
CREATE TABLE sl_fact (id bigint PRIMARY KEY, parent_id bigint, amount int) WITH (autovacuum_enabled = false);
INSERT INTO sl_fact SELECT g, 1 + (g * 7919) % ({rows} / 2), g % 1000 FROM generate_series(1, {rows}) g;
CREATE TABLE sl_members (set_id int, parent_id bigint) WITH (autovacuum_enabled = false);
INSERT INTO sl_members SELECT s, 1 + (s * 104729 + m * 7907) % ({rows} / 2) FROM generate_series(1, 20) s, generate_series(1, 1000) m;
CREATE UNIQUE INDEX sl_members_set ON sl_members (set_id, parent_id);
CREATE INDEX sl_fact_col ON sl_fact USING sift_col (parent_id, amount);
VACUUM (FREEZE, ANALYZE) sl_fact, sl_members
--@ pg
SELECT count(*), sum(amount) FROM sl_fact f WHERE NOT EXISTS (SELECT 1 FROM sl_members m WHERE m.set_id = 3 AND m.parent_id = f.parent_id)
