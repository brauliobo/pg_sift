-- an OR of an inner aggregate and a group column is evaluated for each inner group on its aggregates and keys: the outer aggregate runs in the scan
-- shape: item-summary-rollup
-- expect: agg
-- key: -
SELECT category, kind, avg(avg_price) AS value, count(DISTINCT order_id) AS orders
FROM (SELECT o.id AS order_id, i.category, i.kind, avg(i.price) AS avg_price
      FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
      WHERE o.tenant_id < 120 AND o.deleted_at IS NULL GROUP BY o.id, i.category, i.kind) x
WHERE avg_price > 100.0123 OR kind = 'express'
GROUP BY category, kind ORDER BY category, kind NULLS FIRST
