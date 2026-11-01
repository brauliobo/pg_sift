-- the first page of a list ordered by a scattered key, with the soft-delete test of the table: the page must come from the order array of the key, not from a scan of a third of the table
-- label: `WHERE deleted_at IS NULL ORDER BY k, id LIMIT 20` over 200,000 rows whose key k is a permutation unrelated to the heap order
-- ordered: yes
-- vacuum: no
--@ setup
CREATE TABLE tp_soft (id int PRIMARY KEY, k int NOT NULL, deleted_at timestamptz);
INSERT INTO tp_soft SELECT i, ((i::bigint * 7919) % 200000)::int + 1, NULL FROM generate_series(1, 200000) i;
CREATE INDEX tp_soft_col ON tp_soft USING sift_col (id, k, deleted_at);
VACUUM (FREEZE) tp_soft;
VACUUM (FREEZE, ANALYZE) tp_soft;
DO $$ BEGIN IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_soft' AND attname = 'k') > 0.1 THEN RAISE EXCEPTION 'k follows the heap order'; END IF; END $$
--@ pg
SELECT id, k FROM tp_soft WHERE deleted_at IS NULL ORDER BY k, id LIMIT 20
