-- S2: items at three rollup levels (category + kind, category, all), pre-aggregated per order and averaged over the orders
-- shape: S2
-- expect: any
-- sets: K10 K400 K4000
-- key: -
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, i.category AS mc, i.kind AS mk, {period(o.placed_at)} AS period, i.qty, i.price, i.discount
  FROM orders o
  CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  WHERE o.store_id IN ({stores}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)})
SELECT category, kind, period, avg(avg_price) AS value, count(DISTINCT order_id) AS orders, sum(units) AS units, sum(revenue) AS revenue,
       sum(discounted) AS discounted, sum(cnt) AS items,
       count(DISTINCT order_id) FILTER (WHERE avg_price >= 100) AS high,
       count(DISTINCT order_id) FILTER (WHERE avg_price > 20 AND avg_price < 100) AS mid,
       count(DISTINCT order_id) FILTER (WHERE avg_price <= 20) AS low
FROM (
  SELECT order_id, mc AS category, mk AS kind, period, avg(price) AS avg_price, sum(qty) AS units, sum(qty * (price - discount)) AS revenue,
         count(*) FILTER (WHERE discount > 0) AS discounted, count(*) AS cnt
  FROM base WHERE mc IS NOT NULL GROUP BY order_id, period, mc, mk
  UNION ALL
  SELECT order_id, mc, NULL::text, period, avg(price), sum(qty), sum(qty * (price - discount)), count(*) FILTER (WHERE discount > 0), count(*)
  FROM base WHERE mc IS NOT NULL GROUP BY order_id, period, mc
  UNION ALL
  SELECT order_id, NULL::text, NULL::text, period, avg(price), sum(qty), sum(qty * (price - discount)), count(*) FILTER (WHERE discount > 0), count(*)
  FROM base GROUP BY order_id, period) x
WHERE period IS NOT NULL
GROUP BY category, kind, period
ORDER BY category NULLS FIRST, kind NULLS FIRST, period
