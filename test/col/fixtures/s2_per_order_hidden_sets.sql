-- a rollup over the category above arms with a hidden kind: each set adds the cells of the groups it covers
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, o.country_code AS cc, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.store_id < 120 AND o.deleted_at IS NULL)
SELECT category, GROUPING(category) AS g, count(DISTINCT order_id) AS orders, count(*) AS cells, sum(units) AS units
FROM (SELECT order_id, mc AS category, mk AS kind, sum(qty) AS units FROM base GROUP BY order_id, mc, mk
      UNION ALL
      SELECT order_id, mc, NULL::text, sum(qty) FROM base GROUP BY order_id, mc) x
GROUP BY ROLLUP (category) ORDER BY category NULLS FIRST, g
