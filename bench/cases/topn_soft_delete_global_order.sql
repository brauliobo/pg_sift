-- a first page of a list that excludes the soft-deleted rows (a stamp that is NULL in every row here) is served by the whole order of the key, as without the test
-- label: `WHERE deleted_at IS NULL ORDER BY k, id LIMIT 20` over 120,000 rows whose key k is unrelated to the heap order, no row deleted
-- ordered: yes
-- vacuum: no
-- gap: sift lab 0013 (filtered ordered page), top-N
--@ setup
CREATE TABLE tp_soft (id int PRIMARY KEY, k int NOT NULL, g int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO tp_soft SELECT i, ((i::bigint * 7919) % 120000)::int + 1, i % 1000, NULL FROM generate_series(1, 120000) i;
CREATE INDEX tp_soft_col ON tp_soft USING sift_col (id, k, g, deleted_at);
VACUUM (FREEZE) tp_soft;
VACUUM (FREEZE, ANALYZE) tp_soft;
DO $$ BEGIN IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_soft' AND attname = 'k') > 0.1 THEN RAISE EXCEPTION 'k follows the heap order'; END IF; END $$
--@ pg
SELECT id, k FROM tp_soft WHERE deleted_at IS NULL ORDER BY k, id LIMIT 20
