-- a first page of a list with a condition that few rows pass: the walk of the order array gives up where the units read less, and the planner prices which of the two is cheaper (the variant `walk` forces the walk, `units` the units)
-- label: `WHERE s10k = 1 ORDER BY k, id LIMIT 20` over 300,000 rows whose key k is unrelated to the heap order, 0.01% of the rows pass, after updates that left 20% of the heap pages not all-visible
-- ordered: yes
-- vacuum: no
--@ setup
CREATE EXTENSION IF NOT EXISTS pg_visibility SCHEMA public;
CREATE TABLE tp_pr_10000d (id int PRIMARY KEY, k int NOT NULL, s10k int NOT NULL, g int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tp_pr_10000d SELECT i, ((i::bigint * 7919) % 300000)::int + 1, CASE WHEN i % 10000 = 7 THEN 1 ELSE 0 END, i % 1000 FROM generate_series(1, 300000) i;
CREATE INDEX tp_pr_10000d_col ON tp_pr_10000d USING sift_col (id, k, s10k, g);
VACUUM (FREEZE) tp_pr_10000d;
VACUUM (FREEZE, ANALYZE) tp_pr_10000d;
UPDATE tp_pr_10000d SET g = g + 1 WHERE id IN (SELECT id FROM (SELECT id, (ctid::text::point)[0]::int AS pg, row_number() OVER (PARTITION BY (ctid::text::point)[0]::int ORDER BY id) AS rn FROM tp_pr_10000d) x WHERE pg % 5 = 0 AND rn <= 3);
DO $$ BEGIN
  IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_pr_10000d' AND attname = 'k') > 0.1 THEN RAISE EXCEPTION 'k follows the heap order'; END IF;
  IF (SELECT count(*) FROM tp_pr_10000d WHERE s10k = 1) NOT BETWEEN 25 AND 35 THEN RAISE EXCEPTION 'the condition passes % rows', (SELECT count(*) FROM tp_pr_10000d WHERE s10k = 1); END IF;
  IF (SELECT 1.0 * (pg_relation_size('tp_pr_10000d') / 8192 - all_visible) / (pg_relation_size('tp_pr_10000d') / 8192) FROM public.pg_visibility_map_summary('tp_pr_10000d')) NOT BETWEEN 0.19 AND 0.22 THEN RAISE EXCEPTION 'the pages not all-visible are not 20%% of the heap'; END IF;
END $$
--@ pg
SELECT id, k FROM tp_pr_10000d WHERE s10k = 1 ORDER BY k, id LIMIT 20
