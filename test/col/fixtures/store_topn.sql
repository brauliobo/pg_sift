-- store-top-categories: top 10 (store, category) pairs by item count
-- shape: store-top-categories
-- expect: any
-- sets: K400 K4000
-- key: -
-- scenario: yes
SELECT o.tenant_id, i.category, count(*) AS items, avg(i.price) AS avg_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, price float8)
WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL
GROUP BY o.tenant_id, i.category
ORDER BY items DESC, o.tenant_id, i.category
LIMIT 10
