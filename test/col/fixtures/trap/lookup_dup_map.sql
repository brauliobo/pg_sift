-- trap: a mapping table with two rows for a key replicates every item (and the unmatched ones stay once)
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.tag, count(*) AS n, sum(i.qty) AS qty, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
LEFT JOIN map_dup m ON m.category = i.category
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY m.tag ORDER BY m.tag NULLS FIRST
