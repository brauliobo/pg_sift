-- a group key of the order table next to one of the dimension: the stores without orders are in the NULL status
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, o.status, count(*) AS rows, count(o.id) AS orders, sum(o.total) AS revenue
FROM stores s LEFT JOIN orders o ON o.store_id = s.id AND o.id % 3 = 0
WHERE s.id % 7 = 0
GROUP BY s.kind, o.status ORDER BY s.kind, o.status NULLS FIRST
