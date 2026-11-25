-- item-summary-rollup: the per-order inner aggregate (the order is the group) under an outer aggregate over the orders
-- shape: item-summary-rollup
-- expect: any
-- sets: K400 K4000
-- key: -
SELECT category, count(DISTINCT order_id) AS orders, avg(avg_price) AS avg_of_avg, sum(items) AS items, sum(positive) AS discounted
FROM (SELECT o.id AS order_id, i.category, avg(i.price) AS avg_price, count(*) AS items, count(*) FILTER (WHERE i.discount > 0) AS positive
      FROM orders o, jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
      WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
      GROUP BY o.id, i.category) per_order
GROUP BY category
ORDER BY category
