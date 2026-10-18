-- S2: categories of the orders that are not normalized mapped through categories_map
-- shape: S2
-- expect: any
-- sets: K400 K4000
-- key: -
SELECT CASE WHEN o.attributes->>'normalized' IS NOT NULL THEN i.category ELSE nullif(cm.derived_category, '') END AS category,
       {period(o.placed_at)} AS period, count(DISTINCT o.id) AS orders, sum(i.qty * (i.price - i.discount)) AS revenue,
       avg((o.metrics->>'mhead')::float8) AS mhead
FROM orders o
CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY 1, 2
ORDER BY 1 NULLS FIRST, 2 NULLS FIRST
