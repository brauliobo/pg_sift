-- sift.table_auto_defrag_dead_pct has had no effect since 3.8 (the background pass of a sift_table is asked for by the reads of the table) and is kept so a configuration that sets it loads:
-- SET, RESET and the range are accepted, and a table deleted from by 2% (below any value of it) answers the same with the setting at 1 and at 100.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SHOW sift.table_auto_defrag_dead_pct;
SET sift.table_auto_defrag_dead_pct = 1;
SHOW sift.table_auto_defrag_dead_pct;
SET sift.table_auto_defrag_dead_pct = 100;
SHOW sift.table_auto_defrag_dead_pct;
SET sift.table_auto_defrag_dead_pct = 0;
RESET sift.table_auto_defrag_dead_pct;
CREATE TABLE d (id int, g int, v int) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO d SELECT i, i % 20, i % 1000 FROM generate_series(1, 20000) i;
VACUUM d;
DELETE FROM d WHERE id % 50 = 0;
SET sift.table_auto_defrag_dead_pct = 1;
SELECT g, count(*), sum(v) FROM d GROUP BY g ORDER BY g LIMIT 3;
SET sift.table_auto_defrag_dead_pct = 100;
SELECT g, count(*), sum(v) FROM d GROUP BY g ORDER BY g LIMIT 3;
DROP TABLE d;
DROP EXTENSION pg_sift;
