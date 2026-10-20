-- NOT IN of a sub-select without NULL, over a customer column that has them: a NULL customer is NULL, not true, and never counted
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.deleted_at IS NULL AND o.customer_id NOT IN (SELECT c.customer_id FROM dim_customer c WHERE c.customer_id < 400 AND c.customer_id IS NOT NULL)
GROUP BY o.status ORDER BY o.status NULLS FIRST
