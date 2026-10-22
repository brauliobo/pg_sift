-- trap: a float key with thousands of values next to the store (hash groups), a float key of an object key that some orders lack
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.price, o.tenant_id, (o.metrics->>'m_total')::float8 AS m_total, count(*) AS n, sum(i.qty) AS qty
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
WHERE o.deleted_at IS NULL
GROUP BY i.price, o.tenant_id, (o.metrics->>'m_total')::float8 ORDER BY 1, 2, 3 NULLS FIRST
