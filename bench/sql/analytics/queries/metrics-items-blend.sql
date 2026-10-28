-- metrics-items-blend: a blend of metrics-summary and item-summary-rollup: per mapped item category and span, the revenue of the items and the "m_total" metric of their orders
-- (an array of objects and a path of the metrics object in the same scan)
-- sets: K10 K400 K4000
-- order: 3
-- key: 1 2
--@ pg
SELECT CASE WHEN o.attributes->>'normalized' IS NOT NULL THEN i.category ELSE nullif(cm.derived_category, '') END AS category,
       {span(o.placed_at)} AS span,
       count(DISTINCT o.id) AS orders,
       sum(i.qty * (i.price - i.discount)) AS revenue,
       avg((o.metrics->>'m_total')::float8) AS m_total
FROM orders o
CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY 1, 2
--@ flat
SELECT CASE WHEN i.normalized THEN i.category ELSE nullif(cm.derived_category, '') END AS category,
       {span(i.placed_at)} AS span,
       count(DISTINCT i.order_id) AS orders,
       sum(i.qty * (i.price - i.discount)) AS revenue,
       avg(i.m_total) AS m_total
FROM pg_items i
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE i.tenant_id IN ({tenants}) AND i.source IN ({sources}) AND i.deleted_at IS NULL AND {range(i.placed_at)}
GROUP BY 1, 2
--@ ch
SELECT if(i.normalized != 0, i.category, nullIf(cm.derived_category, '')) AS category,
       {span(i.placed_at)} AS span,
       count(DISTINCT i.order_id) AS orders,
       sum(i.qty * (i.price - i.discount)) AS revenue,
       avg(i.m_total) AS m_total
FROM items_tuned i
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE i.tenant_id IN ({tenants}) AND i.source IN ({sources}) AND i.deleted = 0 AND {range(i.placed_at)}
GROUP BY category, span
SETTINGS join_use_nulls = 1
