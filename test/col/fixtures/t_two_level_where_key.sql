-- the WHERE of the outer query on group keys, in both arms (PostgreSQL pushes it into them): one of the keys is NULL in an arm
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
WHERE category IN ('Toys', 'Home', 'Books') AND kind IS NOT DISTINCT FROM NULL
GROUP BY category, kind ORDER BY category, kind NULLS FIRST
