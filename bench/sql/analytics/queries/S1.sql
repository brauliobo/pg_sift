-- S1: metrics summary per mapped metric, aspect and period (object with dynamic keys, keys mapped through metrics_map)
-- sets: K10 K400 K4000
-- key: 1 2 3
--@ pg
SELECT coalesce(mm.mapped_metric, split_part(kv.key, '_', 1)) AS metric,
       coalesce(mm.mapped_aspect, nullif(split_part(kv.key, '_', 2), '')) AS aspect,
       {period(o.placed_at)} AS period,
       avg((kv.value #>> '{}')::float8) AS avg_value,
       count(DISTINCT o.id) AS orders,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 >= 4) AS high,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 > 3 AND (kv.value #>> '{}')::float8 < 4) AS mid,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 <= 3) AS low
FROM orders o
CROSS JOIN LATERAL jsonb_each(o.metrics) kv
LEFT JOIN metrics_map mm ON mm.source = o.source AND mm.metric_key = kv.key
WHERE o.store_id IN ({stores}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY 1, 2, 3
--@ flat
SELECT coalesce(mm.mapped_metric, split_part(v.metric_key, '_', 1)) AS metric,
       coalesce(mm.mapped_aspect, nullif(split_part(v.metric_key, '_', 2), '')) AS aspect,
       {period(v.placed_at)} AS period,
       avg(v.value) AS avg_value,
       count(DISTINCT v.order_id) AS orders,
       count(DISTINCT v.order_id) FILTER (WHERE v.value >= 4) AS high,
       count(DISTINCT v.order_id) FILTER (WHERE v.value > 3 AND v.value < 4) AS mid,
       count(DISTINCT v.order_id) FILTER (WHERE v.value <= 3) AS low
FROM pg_metrics v
LEFT JOIN metrics_map mm ON mm.source = v.source AND mm.metric_key = v.metric_key
WHERE v.store_id IN ({stores}) AND v.source IN ({sources}) AND v.deleted_at IS NULL AND {range(v.placed_at)}
GROUP BY 1, 2, 3
--@ ch
SELECT coalesce(mm.mapped_metric, splitByChar('_', v.metric_key)[1]) AS metric,
       coalesce(nullIf(mm.mapped_aspect, ''), nullIf(splitByChar('_', v.metric_key)[2], '')) AS aspect,
       {period(v.placed_at)} AS period,
       avg(v.value) AS avg_value,
       count(DISTINCT v.order_id) AS orders,
       countDistinctIf(v.order_id, v.value >= 4) AS high,
       countDistinctIf(v.order_id, v.value > 3 AND v.value < 4) AS mid,
       countDistinctIf(v.order_id, v.value <= 3) AS low
FROM metrics_tuned v
LEFT JOIN metrics_map mm ON mm.source = v.source AND mm.metric_key = v.metric_key
WHERE v.store_id IN ({stores}) AND v.source IN ({sources}) AND v.deleted = 0 AND {range(v.placed_at)}
GROUP BY metric, aspect, period
SETTINGS join_use_nulls = 1
