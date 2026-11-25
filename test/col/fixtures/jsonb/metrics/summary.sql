-- metrics-summary: metrics summary per metric, part and span (an object with dynamic keys: jsonb_each), the thresholds counted by distinct orders
-- shape: metrics-summary
-- expect: agg
-- sets: K10 K400 K4000
-- key: -
-- scenario: yes
SELECT split_part(kv.key, '_', 1) AS metric,
       nullif(split_part(kv.key, '_', 2), '') AS part,
       {span(o.placed_at)} AS span,
       avg((kv.value #>> '{}')::float8) AS avg_value,
       count(DISTINCT o.id) AS orders,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 >= 4) AS high,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 > 3 AND (kv.value #>> '{}')::float8 < 4) AS mid,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 <= 3) AS low
FROM orders o
CROSS JOIN LATERAL jsonb_each(o.metrics) kv
WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3
