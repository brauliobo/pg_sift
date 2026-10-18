-- S1: the same with the keys mapped through metrics_map (a left join with the fallback to the split of the key)
-- shape: S1
-- expect: any
-- sets: K400 K4000
-- key: -
-- scenario: yes
SELECT coalesce(mm.mapped_metric, split_part(kv.key, '_', 1)) AS metric,
       coalesce(mm.mapped_aspect, nullif(split_part(kv.key, '_', 2), '')) AS aspect,
       {period(o.placed_at)} AS period,
       avg((kv.value #>> '{}')::float8) AS avg_value,
       count(DISTINCT o.id) AS orders,
       count(DISTINCT o.id) FILTER (WHERE (kv.value #>> '{}')::float8 >= 4) AS high
FROM orders o
CROSS JOIN LATERAL jsonb_each(o.metrics) kv
LEFT JOIN metrics_map mm ON mm.source = o.source AND mm.metric_key = kv.key
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY 1, 2, 3
ORDER BY 1, 2, 3
