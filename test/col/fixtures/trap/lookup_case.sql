-- trap: keys that differ by case only are different keys (the items of the orders that are not normalized have other cases)
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, m.tag, count(*) AS n
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_case m ON m.category = i.category
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY i.category, m.tag ORDER BY 1, 2 NULLS FIRST
