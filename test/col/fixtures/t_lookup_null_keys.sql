-- trap: NULL keys on both sides never match (the items without a kind, the NULL key of the mapping table)
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.kind, m.tag, count(*) AS n
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_null m ON m.category = i.kind
WHERE o.store_id < 120 AND o.deleted_at IS NULL
GROUP BY i.kind, m.tag ORDER BY 1 NULLS FIRST, 2 NULLS FIRST
