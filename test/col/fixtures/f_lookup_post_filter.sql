-- a condition on the mapped column filters the joined rows
-- shape: flat
-- expect: any
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS n, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_dup m ON m.category = i.category
WHERE o.store_id < 120 AND o.deleted_at IS NULL AND (m.tag IS NULL OR m.tag = 'b')
GROUP BY i.category ORDER BY 1
