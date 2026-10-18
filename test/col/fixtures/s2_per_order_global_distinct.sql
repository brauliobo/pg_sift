-- distinct orders over the arms with no outer key: the arms group by category and kind too, so a row has several cells (inner groups) in the one group
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, o.country_code AS cc, i.category AS mc, i.kind AS mk, i.qty, i.price
  FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id < 120 AND o.deleted_at IS NULL)
SELECT count(DISTINCT order_id) AS orders, count(*) AS cells, sum(units) AS units, avg(avg_price) AS value,
       count(DISTINCT order_id) FILTER (WHERE avg_price >= 100.0123) AS high, count(*) FILTER (WHERE units > 3) AS big
FROM (SELECT order_id, mc AS category, mk AS kind, avg(price) AS avg_price, sum(qty) AS units FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc, mk
      UNION ALL
      SELECT order_id, mc, NULL::text, avg(price), sum(qty) FROM base WHERE mc IS NOT NULL GROUP BY order_id, mc) x
