-- the first page of a list after a few scattered updates left their heap pages not all-visible: the ordered walk must check the rows of the page, not every row of the units that hold a changed page
-- label: `ORDER BY k, id LIMIT 20` over 200,000 rows after `UPDATE .. WHERE id % 997 = 0` (about 200 rows on 200 of 1,700 pages), key k unrelated to the heap order
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE tp_upd (id int PRIMARY KEY, k int NOT NULL, v int NOT NULL);
INSERT INTO tp_upd SELECT i, ((i::bigint * 7919) % 200000)::int + 1, i % 100 FROM generate_series(1, 200000) i;
CREATE INDEX tp_upd_col ON tp_upd USING sift_col (id, k, v);
VACUUM (FREEZE) tp_upd;
VACUUM (FREEZE, ANALYZE) tp_upd;
DO $$ BEGIN IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_upd' AND attname = 'k') > 0.1 THEN RAISE EXCEPTION 'k follows the heap order'; END IF; END $$;
UPDATE tp_upd SET v = v + 1 WHERE id % 997 = 0
--@ pg
SELECT id, k FROM tp_upd ORDER BY k, id LIMIT 20
