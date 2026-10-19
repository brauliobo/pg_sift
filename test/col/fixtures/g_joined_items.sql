-- generic aggregates over a mapped column of the unnested items, with a leaf next to it in the same call
-- shape: S2
-- expect: agg
-- key: -
SELECT o.source, string_agg(DISTINCT coalesce(cm.derived_category, '?'), ',') AS cats, array_agg(cm.derived_category ORDER BY o.id, i.sku, i.category) FILTER (WHERE o.tenant_id < 6) AS flag_a,
       min(cm.derived_category || i.kind) AS lo, stddev_samp(length(cm.derived_category) * i.qty) AS sd
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, sku text, kind text, qty int, price float8, discount float8)
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE o.tenant_id < 40 GROUP BY o.source ORDER BY o.source
