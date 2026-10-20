-- the dimension is the preserved side of a LEFT JOIN: a store without an order is a group of its own, count(o.id) is 0, count(*) is 1 and the sum is NULL
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.id, s.kind, count(o.id) AS orders, count(*) AS rows, sum(o.total) AS revenue, min(o.placed_at) AS first_day
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id
WHERE s.id > 340
GROUP BY s.id, s.kind ORDER BY s.id
