-- a condition over a leaf of the order and a leaf of its items
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS n, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL AND (o.source = 'web' OR i.price > 100)
GROUP BY i.category ORDER BY 1
