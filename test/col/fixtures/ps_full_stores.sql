-- the J06 sample: FULL JOIN of the orders and their stores
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS orders, count(*) AS rows, sum(o.total) AS revenue
FROM orders o FULL JOIN stores s ON s.id = o.store_id AND o.total < 200
GROUP BY s.kind ORDER BY s.kind NULLS FIRST
