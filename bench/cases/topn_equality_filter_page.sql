-- a page of a list ordered by a scattered key within one group: an equality on a column and the order of another, the shape of a per-owner list
-- label: `WHERE grp = 17 ORDER BY k, id LIMIT 20` over 200,000 rows in 1,000 groups, key k unrelated to the heap order
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE tp_group (id int PRIMARY KEY, k int NOT NULL, grp int NOT NULL);
INSERT INTO tp_group SELECT i, ((i::bigint * 7919) % 200000)::int + 1, (hashint4(i) & 16777215) % 1000 FROM generate_series(1, 200000) i;
CREATE INDEX tp_group_col ON tp_group USING sift_col (id, k, grp);
VACUUM (FREEZE) tp_group;
VACUUM (FREEZE, ANALYZE) tp_group;
DO $$ BEGIN IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_group' AND attname = 'k') > 0.1 THEN RAISE EXCEPTION 'k follows the heap order'; END IF; END $$
--@ pg
SELECT id, k FROM tp_group WHERE grp = 17 ORDER BY k, id LIMIT 20
