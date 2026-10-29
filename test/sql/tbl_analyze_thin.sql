-- ANALYZE of a sift_table gives the statistics of the rows the table holds although it hands the sampler only a share of the rows of a large
-- table (the sampler keeps 30,000 of them): the row count, the distinct values of a small domain, the most common value and the correlation
-- of a column in the order of the table are those of a heap twin
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int, grp int, status text, v int) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE h (LIKE t) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT g, g % 7, CASE WHEN g % 10 < 6 THEN 'paid' WHEN g % 10 < 9 THEN 'sent' ELSE 'lost' END, g FROM generate_series(1, 600000) g;
SELECT sift.table_maintain('t') IS NOT NULL AS maintained;
INSERT INTO h SELECT * FROM t;
DELETE FROM t WHERE id % 4 = 0;
DELETE FROM h WHERE id % 4 = 0;
ANALYZE t, h;
SELECT abs(reltuples - 450000) < 450000 * 0.03 AS rows_within_3_percent FROM pg_class WHERE relname = 't';
SELECT attname, (n_distinct = 7) AS seven_groups, (n_distinct = 3) AS three_statuses
FROM pg_stats WHERE tablename = 't' AND attname IN ('grp', 'status') ORDER BY attname;
SELECT abs((SELECT most_common_freqs[1] FROM pg_stats WHERE tablename = 't' AND attname = 'status')
         - (SELECT most_common_freqs[1] FROM pg_stats WHERE tablename = 'h' AND attname = 'status')) < 0.02 AS paid_share_like_heap,
       abs((SELECT correlation FROM pg_stats WHERE tablename = 't' AND attname = 'id')) > 0.9 AS id_in_table_order;
DROP TABLE t, h;
DROP EXTENSION pg_sift;
