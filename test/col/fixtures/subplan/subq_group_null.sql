-- the sub-select groups a column with NULLs: its NULL group has a row, which matches nothing (a NULL key)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT c.orders_of_customer, count(*) AS n, sum(o.total) AS revenue
FROM orders o LEFT JOIN (SELECT customer_id, count(*) AS orders_of_customer FROM orders_odd GROUP BY customer_id) c ON o.customer_id = c.customer_id
WHERE o.deleted_at IS NULL
GROUP BY c.orders_of_customer ORDER BY c.orders_of_customer NULLS FIRST
