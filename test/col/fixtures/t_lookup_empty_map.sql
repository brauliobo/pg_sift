-- trap: an empty mapping table: every key is unmatched
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.tag, count(*) AS n, sum(i.qty) AS qty
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_empty m ON m.category = i.category
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY m.tag ORDER BY 1 NULLS FIRST
