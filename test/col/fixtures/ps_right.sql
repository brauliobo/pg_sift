-- RIGHT JOIN: the same rows as the LEFT JOIN from the dimension
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS orders, count(*) AS rows, sum(o.total) AS revenue
FROM orders o RIGHT JOIN stores s ON s.id = o.store_id AND o.deleted_at IS NULL
GROUP BY s.kind ORDER BY s.kind
