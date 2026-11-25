-- prepared statement with a generic plan: the parameters are Params in the plan
-- shape: prepared
-- expect: any
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (int[], date, text)
-- args: ARRAY[1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233], DATE '2024-01-01', 'cancelled'
SELECT source, count(*) AS n, sum(tenant_id) AS s FROM orders WHERE tenant_id = ANY ($1) AND placed_at >= $2 AND status <> $3 GROUP BY source ORDER BY source
