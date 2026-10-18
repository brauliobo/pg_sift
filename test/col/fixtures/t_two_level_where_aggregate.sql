-- the WHERE of the outer query on inner aggregates is the HAVING of the arms: the per-order groups that fail it are not groups
-- shape: S2
-- expect: agg
-- key: -
WITH base AS (
  SELECT o.id AS order_id, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.store_id < 120 AND o.deleted_at IS NULL)
SELECT category, kind, avg(avg_price) AS value, count(DISTINCT order_id) AS orders, sum(units) AS units
FROM (SELECT order_id, mc AS category, mk AS kind, avg(price) AS avg_price, sum(qty) AS units FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc, mk
      UNION ALL
      SELECT order_id, mc, NULL::text, avg(price), sum(qty) FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc) x
WHERE avg_price > 40 AND units >= 2
GROUP BY category, kind ORDER BY category, kind NULLS FIRST
