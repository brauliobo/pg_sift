-- item-summary-rollup: counts, FILTER thresholds and averages per category over the expanded items
-- shape: item-summary-rollup
-- expect: agg
-- sets: K10 K400 K4000
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS items, count(*) FILTER (WHERE i.discount > 0) AS discounted, count(*) FILTER (WHERE i.price >= 100) AS pricey,
       avg(i.price) AS avg_price, sum(i.qty) AS units, sum(i.qty * (i.price - i.discount)) AS revenue, min(i.price) AS lo, max(i.price) AS hi
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY i.category
ORDER BY i.category
