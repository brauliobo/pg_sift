-- cost: a selective set of orders with an unnest, a lookup join and a metric key takes the scan (priced 408 against 528 for the normal plan; 0.29 ms against 0.33 ms, planning included)
-- shape: S3
-- expect: agg
-- key: -
SELECT CASE WHEN o.attributes->>'normalized' IS NOT NULL THEN i.category ELSE nullif(cm.derived_category, '') END AS category,
       count(DISTINCT o.id) AS orders, sum(i.qty * (i.price - i.discount)) AS revenue, avg((o.metrics->>'m_total')::float8) AS m_total
FROM orders o
CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
LEFT JOIN categories_map cm ON cm.category = i.category
WHERE o.id < 40 AND o.deleted_at IS NULL
GROUP BY 1 ORDER BY 1
