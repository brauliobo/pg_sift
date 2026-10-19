-- cost: a selective set of orders with an unnest, a lookup join and a metric key keeps the normal plan (its estimate of 100 rows per call is not a reason to scan)
-- shape: S3
-- expect: plain
-- key: -
SELECT CASE WHEN o.attributes->>'normalized' IS NOT NULL THEN i.category ELSE nullif(cm.mapped_category, '') END AS category,
       count(DISTINCT o.id) AS orders, sum(i.qty * (i.price - i.discount)) AS revenue, avg((o.metrics->>'overall')::float8) AS overall
FROM orders o
CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE o.id < 40 AND o.deleted_at IS NULL
GROUP BY 1 ORDER BY 1
