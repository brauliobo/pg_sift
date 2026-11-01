-- a first page of a list with the soft-delete test (so no whole order) reads the heap pages of its 20 rows, not every row of the pages that are not all-visible (autovacuum off, 5% of the pages changed)
-- label: `WHERE deleted_at IS NULL ORDER BY k, id LIMIT 20` over 120,000 rows whose key k is unrelated to the heap order, after updates that left 5% of the heap pages not all-visible
-- ordered: yes
-- vacuum: no
-- gap: sift lab 0004 / 0013 (rows of dirty pages vetted for the page only)
--@ setup
CREATE EXTENSION IF NOT EXISTS pg_visibility SCHEMA public;
CREATE TABLE tp_d5 (id int PRIMARY KEY, k int NOT NULL, g int NOT NULL, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO tp_d5 SELECT i, ((i::bigint * 7919) % 120000)::int + 1, i % 1000, NULL FROM generate_series(1, 120000) i;
CREATE INDEX tp_d5_col ON tp_d5 USING sift_col (id, k, g, deleted_at);
VACUUM (FREEZE) tp_d5;
VACUUM (FREEZE, ANALYZE) tp_d5;
UPDATE tp_d5 SET g = g + 1 WHERE id IN (SELECT id FROM (SELECT id, (ctid::text::point)[0]::int AS pg, row_number() OVER (PARTITION BY (ctid::text::point)[0]::int ORDER BY id) AS rn FROM tp_d5) x WHERE pg % 20 = 0 AND rn <= 3);
DO $$ DECLARE dirty numeric; BEGIN
  IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_d5' AND attname = 'k') > 0.1 THEN RAISE EXCEPTION 'k follows the heap order'; END IF;
  SELECT 1.0 * (pg_relation_size('tp_d5') / 8192 - all_visible) / (pg_relation_size('tp_d5') / 8192) INTO dirty FROM public.pg_visibility_map_summary('tp_d5');
  IF dirty NOT BETWEEN 0.04 AND 0.07 THEN RAISE EXCEPTION 'the pages not all-visible are % of the heap', dirty; END IF;
END $$
--@ pg
SELECT id, k FROM tp_d5 WHERE deleted_at IS NULL ORDER BY k, id LIMIT 20
