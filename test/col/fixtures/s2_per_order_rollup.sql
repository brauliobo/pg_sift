-- the rollup levels of the per-order aggregates in one ROLLUP over one arm: every set adds the inner groups its keys cover
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT category, kind, GROUPING(category, kind) AS g, avg(avg_price) AS value, count(DISTINCT order_id) AS orders, sum(units) AS units,
       count(*) AS cells, count(DISTINCT order_id) FILTER (WHERE avg_price >= 100.0123) AS high
FROM (SELECT order_id, mc AS category, mk AS kind, avg(price) AS avg_price, sum(qty) AS units FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc, mk) x
GROUP BY ROLLUP (category, kind) ORDER BY g, category NULLS FIRST, kind NULLS FIRST
