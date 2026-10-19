-- one arm of per-order aggregates under the averages over the orders: one scan, not a row per order and category
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT category, kind, avg(avg_price) AS value, count(DISTINCT order_id) AS orders, sum(units) AS units
FROM (SELECT o.id AS order_id, i.category AS category, i.kind AS kind, avg(i.price) AS avg_price, sum(i.qty) AS units
      FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
      WHERE o.store_id < 120 AND o.deleted_at IS NULL AND i.category IS NOT NULL GROUP BY o.id, i.category, i.kind) x
GROUP BY category, kind ORDER BY category, kind NULLS FIRST
