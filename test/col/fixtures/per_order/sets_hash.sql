-- grouping sets over per-order aggregates when the key ids outgrow the direct arrays (the groups of the sets are made in the hash table)
-- shape: item-summary-rollup
-- expect: agg
-- key: -
WITH base AS (
  SELECT o.id AS order_id, o.placed_at, o.source, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.deleted_at IS NULL)
SELECT category, kind, placed_at, source, GROUPING(category, kind, placed_at, source) AS g, avg(avg_price) AS value, count(DISTINCT order_id) AS orders,
       sum(units) AS units, count(*) AS cells
FROM (SELECT order_id, placed_at, source, mc AS category, mk AS kind, avg(price) AS avg_price, sum(qty) AS units FROM base GROUP BY order_id, placed_at, source, mc, mk) x
GROUP BY GROUPING SETS ((category, kind, placed_at, source), (category, kind, placed_at), (category, placed_at), (source), (category), ())
ORDER BY g, category NULLS FIRST, kind NULLS FIRST, placed_at NULLS FIRST, source NULLS FIRST
