-- S3: a blend of S1 and S2: per mapped item category and period, the revenue of the items and the "overall" metric of their orders
-- (an array of objects and a path of the metrics object in the same scan)
-- sets: K10 K400 K4000
-- key: 1 2
--@ pg
SELECT CASE WHEN o.attributes->>'normalized' IS NOT NULL THEN i.category ELSE nullif(cm.mapped_category, '') END AS category,
       {period(o.placed_at)} AS period,
       count(DISTINCT o.id) AS orders,
       sum(i.qty * (i.price - i.discount)) AS revenue,
       avg((o.metrics->>'overall')::float8) AS overall
FROM orders o
CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE o.store_id IN ({stores}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY 1, 2
--@ flat
SELECT CASE WHEN i.normalized THEN i.category ELSE nullif(cm.mapped_category, '') END AS category,
       {period(i.placed_at)} AS period,
       count(DISTINCT i.order_id) AS orders,
       sum(i.qty * (i.price - i.discount)) AS revenue,
       avg(i.overall) AS overall
FROM pg_items i
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE i.store_id IN ({stores}) AND i.source IN ({sources}) AND i.deleted_at IS NULL AND {range(i.placed_at)}
GROUP BY 1, 2
--@ ch
SELECT if(i.normalized != 0, i.category, nullIf(cm.mapped_category, '')) AS category,
       {period(i.placed_at)} AS period,
       count(DISTINCT i.order_id) AS orders,
       sum(i.qty * (i.price - i.discount)) AS revenue,
       avg(i.overall) AS overall
FROM items_tuned i
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE i.store_id IN ({stores}) AND i.source IN ({sources}) AND i.deleted = 0 AND {range(i.placed_at)}
GROUP BY category, period
SETTINGS join_use_nulls = 1
