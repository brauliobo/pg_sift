-- a partitioned table (the index is per partition), with pruning
-- shape: part
-- expect: agg
-- key: -
SELECT date_trunc('year', placed_at)::date AS year, count(*) AS n, count(DISTINCT store_id) AS stores, sum(store_id) AS s
FROM orders_part WHERE placed_at >= DATE '2023-06-01' GROUP BY 1 ORDER BY 1
