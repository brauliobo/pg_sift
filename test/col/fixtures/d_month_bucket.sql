-- distinct stores and orders per month: the shape of "customers per month", groups that follow the heap order
-- shape: distinct
-- expect: agg
-- key: -
-- scenario: yes
SELECT date_trunc('month', placed_at)::date AS month, count(DISTINCT store_id) AS stores, count(DISTINCT id) AS orders
FROM orders WHERE deleted_at IS NULL GROUP BY 1 ORDER BY 1
