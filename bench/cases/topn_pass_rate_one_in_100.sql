-- a first page of a list with a condition that few rows pass: the walk of the order array gives up where the units read less, and the planner prices which of the two is cheaper (the variant `walk` forces the walk, `units` the units)
-- label: `WHERE s100 = 1 ORDER BY k, id LIMIT 20` over 300,000 rows whose key k is unrelated to the heap order, 1% of the rows pass
-- ordered: yes
-- vacuum: no
--@ setup
CREATE EXTENSION IF NOT EXISTS pg_visibility SCHEMA public;
CREATE TABLE tp_pr_100f (id int PRIMARY KEY, k int NOT NULL, s100 int NOT NULL, g int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO tp_pr_100f SELECT i, ((i::bigint * 7919) % 300000)::int + 1, CASE WHEN i % 100 = 7 THEN 1 ELSE 0 END, i % 1000 FROM generate_series(1, 300000) i;
CREATE INDEX tp_pr_100f_col ON tp_pr_100f USING sift_col (id, k, s100, g);
VACUUM (FREEZE) tp_pr_100f;
VACUUM (FREEZE, ANALYZE) tp_pr_100f;
DO $$ BEGIN
  IF (SELECT abs(correlation) FROM pg_stats WHERE tablename = 'tp_pr_100f' AND attname = 'k') > 0.1 THEN RAISE EXCEPTION 'k follows the heap order'; END IF;
  IF (SELECT count(*) FROM tp_pr_100f WHERE s100 = 1) NOT BETWEEN 2900 AND 3100 THEN RAISE EXCEPTION 'the condition passes % rows', (SELECT count(*) FROM tp_pr_100f WHERE s100 = 1); END IF;
END $$
--@ pg
SELECT id, k FROM tp_pr_100f WHERE s100 = 1 ORDER BY k, id LIMIT 20
