-- a range lookup grouped by its label, over the unnested items
-- shape: flat
-- expect: agg
-- key: 1
-- scenario: yes
SELECT p.label, count(DISTINCT o.id) AS orders, avg(i.price) AS price, sum(i.qty) AS qty
FROM orders o JOIN spans p ON o.placed_at <@ p.range
CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8)
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY p.label ORDER BY 1 NULLS FIRST
