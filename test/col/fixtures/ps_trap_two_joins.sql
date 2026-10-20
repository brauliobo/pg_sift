-- trap: a second join with the order table keeps the normal plan
-- shape: trap
-- expect: plain
-- key: -
-- scenario: yes
SELECT s.kind, c.segment, count(o.id) AS orders, count(*) AS rows
FROM stores s LEFT JOIN orders o ON o.store_id = s.id LEFT JOIN dim_customer c ON c.customer_id = o.customer_id
GROUP BY s.kind, c.segment ORDER BY s.kind, c.segment NULLS FIRST
