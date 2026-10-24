-- S1: rollups of metric and part with GROUPING SETS
-- shape: S1
-- expect: any
-- sets: K400
-- key: -
SELECT split_part(kv.key, '_', 1) AS metric, nullif(split_part(kv.key, '_', 2), '') AS part, grouping(split_part(kv.key, '_', 1), nullif(split_part(kv.key, '_', 2), '')) AS lvl,
       avg((kv.value #>> '{}')::float8) AS avg_value, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_each(o.metrics) kv
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
GROUP BY GROUPING SETS ((1, 2), (1), ())
ORDER BY 3, 1, 2
