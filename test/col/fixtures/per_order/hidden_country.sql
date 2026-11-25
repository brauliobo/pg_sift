-- distinct orders by a column of the orders (the arms group by it and by category): two arms, the second without the category
-- shape: item-summary-rollup
-- expect: agg
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, o.country_code AS cc, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT cc, count(DISTINCT order_id) AS orders, count(*) AS cells, sum(units) AS units
FROM (SELECT order_id, cc, mc AS category, sum(qty) AS units FROM base GROUP BY order_id, cc, mc
      UNION ALL
      SELECT order_id, cc, NULL::text, sum(qty) FROM base GROUP BY order_id, cc) x
GROUP BY cc ORDER BY cc NULLS FIRST
