-- FULL JOIN with rows unmatched on both sides: orders with a NULL customer or a customer without a row (NULL segment), customers whose orders fail the ON clause
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT c.segment, count(o.id) AS orders, count(c.customer_id) AS customers, count(*) AS rows, sum(o.total) AS revenue
FROM orders o FULL JOIN dim_customer c ON c.customer_id = o.customer_id AND o.total > 400
GROUP BY c.segment ORDER BY c.segment NULLS FIRST
