-- metrics-summary: metrics summary per mapped metric, part and span (object with dynamic keys, keys mapped through metrics_map)
-- sets: K10 K400 K4000
-- order: 1
-- key: 1 2 3
--@ pg
SELECT coalesce(mm.derived_metric, split_part(kv.key, '_', 1)) AS metric,
       coalesce(mm.derived_part, nullif(split_part(kv.key, '_', 2), '')) AS part,
       {span(o.placed_at)} AS span,
       avg((kv.value #>> '{}')::float8) AS avg_value,
       count(DISTINCT o.id) AS orders,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 >= 4) AS high,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 > 3 AND (kv.value #>> '{}')::float8 < 4) AS mid,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 <= 3) AS low
FROM orders o
CROSS JOIN LATERAL jsonb_each(o.metrics) kv
LEFT JOIN metrics_map mm ON mm.source = o.source AND mm.metric_key = kv.key
WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY 1, 2, 3
--@ flat
SELECT coalesce(mm.derived_metric, split_part(v.metric_key, '_', 1)) AS metric,
       coalesce(mm.derived_part, nullif(split_part(v.metric_key, '_', 2), '')) AS part,
       {span(v.placed_at)} AS span,
       avg(v.value) AS avg_value,
       count(DISTINCT v.order_id) AS orders,
       count(DISTINCT v.order_id) FILTER (WHERE v.value >= 4) AS high,
       count(DISTINCT v.order_id) FILTER (WHERE v.value > 3 AND v.value < 4) AS mid,
       count(DISTINCT v.order_id) FILTER (WHERE v.value <= 3) AS low
FROM pg_metrics v
LEFT JOIN metrics_map mm ON mm.source = v.source AND mm.metric_key = v.metric_key
WHERE v.tenant_id IN ({tenants}) AND v.source IN ({sources}) AND v.deleted_at IS NULL AND {range(v.placed_at)}
GROUP BY 1, 2, 3
--@ ch
SELECT coalesce(mm.derived_metric, splitByChar('_', v.metric_key)[1]) AS metric,
       coalesce(nullIf(mm.derived_part, ''), nullIf(splitByChar('_', v.metric_key)[2], '')) AS part,
       {span(v.placed_at)} AS span,
       avg(v.value) AS avg_value,
       count(DISTINCT v.order_id) AS orders,
       countDistinctIf(v.order_id, v.value >= 4) AS high,
       countDistinctIf(v.order_id, v.value > 3 AND v.value < 4) AS mid,
       countDistinctIf(v.order_id, v.value <= 3) AS low
FROM metrics_tuned v
LEFT JOIN metrics_map mm ON mm.source = v.source AND mm.metric_key = v.metric_key
WHERE v.tenant_id IN ({tenants}) AND v.source IN ({sources}) AND v.deleted = 0 AND {range(v.placed_at)}
GROUP BY metric, part, span
SETTINGS join_use_nulls = 1
