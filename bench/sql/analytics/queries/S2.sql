-- S2: item summary at three rollup levels (category + kind, category, all), pre-aggregated per order and then averaged over orders;
-- categories and kinds of the orders that are not normalized are mapped through categories_map and kinds_map
-- sets: K10 K400 K4000
-- key: 1 2 3
--@ pg
WITH base AS (
  SELECT o.id AS order_id,
         CASE WHEN o.attributes->>'normalized' IS NOT NULL THEN i.category ELSE nullif(cm.mapped_category, '') END AS mc,
         CASE WHEN o.attributes->>'normalized' IS NOT NULL THEN i.kind ELSE nullif(km.mapped_kind, '') END AS mk,
         {span(o.placed_at)} AS span, i.qty, i.price, i.discount
  FROM orders o
  CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
  LEFT JOIN categories_map cm ON cm.category = i.category
  LEFT JOIN kinds_map km ON km.kind = i.kind
  WHERE o.store_id IN ({stores}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)})
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
GROUP BY category, kind, span
--@ flat
WITH base AS (
  SELECT i.order_id,
         CASE WHEN i.normalized THEN i.category ELSE nullif(cm.mapped_category, '') END AS mc,
         CASE WHEN i.normalized THEN i.kind ELSE nullif(km.mapped_kind, '') END AS mk,
         {span(i.placed_at)} AS span, i.qty, i.price, i.discount
  FROM pg_items i
  LEFT JOIN categories_map cm ON cm.category = i.category
  LEFT JOIN kinds_map km ON km.kind = i.kind
  WHERE i.store_id IN ({stores}) AND i.source IN ({sources}) AND i.deleted_at IS NULL AND {range(i.placed_at)})
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
GROUP BY category, kind, span
--@ ch
WITH base AS (
  SELECT i.order_id AS order_id,
         if(i.normalized != 0, i.category, nullIf(cm.mapped_category, '')) AS mc,
         if(i.normalized != 0, i.kind, nullIf(km.mapped_kind, '')) AS mk,
         {span(i.placed_at)} AS span, i.qty AS qty, i.price AS price, i.discount AS discount
  FROM items_tuned i
  LEFT JOIN categories_map cm ON cm.category = i.category
  LEFT JOIN kinds_map km ON km.kind = i.kind
  WHERE i.store_id IN ({stores}) AND i.source IN ({sources}) AND i.deleted = 0 AND {range(i.placed_at)})
SELECT category, kind, span, avg(avg_price) AS value, count(DISTINCT order_id) AS orders, sum(units) AS units, sum(revenue) AS revenue,
       sum(discounted) AS discounted, sum(cnt) AS items,
       countDistinctIf(order_id, avg_price >= 100) AS high,
       countDistinctIf(order_id, avg_price > 20 AND avg_price < 100) AS mid,
       countDistinctIf(order_id, avg_price <= 20) AS low
FROM (
  SELECT order_id, mc AS category, mk AS kind, span, avg(price) AS avg_price, sum(qty) AS units, sum(qty * (price - discount)) AS revenue,
         countIf(discount > 0) AS discounted, count(*) AS cnt
  FROM base WHERE mc IS NOT NULL GROUP BY order_id, span, mc, mk
  UNION ALL
  SELECT order_id, mc, CAST(NULL, 'Nullable(String)'), span, avg(price), sum(qty), sum(qty * (price - discount)), countIf(discount > 0), count(*)
  FROM base WHERE mc IS NOT NULL GROUP BY order_id, span, mc
  UNION ALL
  SELECT order_id, CAST(NULL, 'Nullable(String)'), CAST(NULL, 'Nullable(String)'), span, avg(price), sum(qty), sum(qty * (price - discount)), countIf(discount > 0), count(*)
  FROM base GROUP BY order_id, span) x
GROUP BY category, kind, span
SETTINGS join_use_nulls = 1
