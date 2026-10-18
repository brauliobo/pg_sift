-- S4: orders per month, with the distinct stores and the sum of a column
-- shape: S4
-- expect: agg
-- sets: K10 K400 K4000
-- key: -
-- scenario: yes
SELECT date_trunc('month', o.placed_at)::date AS month, count(*) AS orders, count(DISTINCT o.store_id) AS stores, sum(o.store_id) AS store_sum, count(o.customer_id) AS customers
FROM orders o
WHERE o.store_id IN ({stores}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
GROUP BY 1
ORDER BY 1
