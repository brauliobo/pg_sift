-- trap: a partitioned table (the index is per partition), with pruning
-- shape: trap
-- expect: any
-- key: -
SELECT date_trunc('year', placed_at)::date AS year, count(*) AS n, count(DISTINCT tenant_id) AS tenants, sum(tenant_id) AS s
FROM orders_part WHERE placed_at >= DATE '2023-06-01' GROUP BY 1 ORDER BY 1
