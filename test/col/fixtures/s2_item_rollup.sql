-- S2: items at three rollup levels (category + kind, category, all), pre-aggregated per order and averaged over the orders
-- shape: S2
-- expect: any
-- sets: K10 K400 K4000
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, i.category AS mc, i.kind AS mk, {span(o.placed_at)} AS span, i.qty, i.price, i.discount
  FROM orders o
  CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)})
SELECT category, kind, span, avg(avg_price) AS value, count(DISTINCT order_id) AS orders, sum(units) AS units, sum(revenue) AS revenue,
       sum(discounted) AS discounted, sum(cnt) AS items,
       count(DISTINCT order_id) FILTER (WHERE avg_price >= 100) AS high,
       count(DISTINCT order_id) FILTER (WHERE avg_price > 20 AND avg_price < 100) AS mid,
       count(DISTINCT order_id) FILTER (WHERE avg_price <= 20) AS low
FROM (
  SELECT order_id, mc AS category, mk AS kind, span, avg(price) AS avg_price, sum(qty) AS units, sum(qty * (price - discount)) AS revenue,
         count(*) FILTER (WHERE discount > 0) AS discounted, count(*) AS cnt
  FROM base WHERE mc IS NOT NULL GROUP BY order_id, span, mc, mk
  UNION ALL
  SELECT order_id, mc, NULL::text, span, avg(price), sum(qty), sum(qty * (price - discount)), count(*) FILTER (WHERE discount > 0), count(*)
  FROM base WHERE mc IS NOT NULL GROUP BY order_id, span, mc
  UNION ALL
  SELECT order_id, NULL::text, NULL::text, span, avg(price), sum(qty), sum(qty * (price - discount)), count(*) FILTER (WHERE discount > 0), count(*)
  FROM base GROUP BY order_id, span) x
WHERE span IS NOT NULL
GROUP BY category, kind, span
ORDER BY category NULLS FIRST, kind NULLS FIRST, span
