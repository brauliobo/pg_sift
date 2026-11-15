-- SiftAgg over a sift_table that has no page yet (created, nothing inserted): the column source copies the write buffers of the table, and there are none.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
CREATE TABLE agg_empty (id int, g int, v bigint) USING sift_table;
SET sift.enable_agg = on;
SELECT count(*), sum(v) FROM agg_empty;
SELECT g, count(*) FROM agg_empty GROUP BY g;
SET sift.enable_agg = off;
SELECT count(*), sum(v) FROM agg_empty;
DROP TABLE agg_empty;
DROP EXTENSION pg_sift;
