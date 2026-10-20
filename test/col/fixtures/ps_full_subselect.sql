-- a sub-select PostgreSQL keeps (LIMIT) is the side a FULL JOIN preserves: the orders of the other stores are rows of NULL columns
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS rows, count(o.id) AS orders, count(s.id) AS stores, sum(o.total) AS revenue
FROM orders o FULL JOIN (SELECT id, kind FROM stores ORDER BY id LIMIT 7) s ON o.store_id = s.id AND o.id % 4 = 0
GROUP BY s.kind ORDER BY s.kind NULLS FIRST
