-- trap: see t_lookup_missing_rows, as an inner join
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.tag, count(*) AS n, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
JOIN map_part m ON m.category = i.category
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY m.tag ORDER BY 1
