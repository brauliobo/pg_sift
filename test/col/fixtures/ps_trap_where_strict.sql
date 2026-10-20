-- trap: a strict WHERE on the order table turns the join into an inner one, whose rows the dimension does not preserve
-- shape: trap
-- expect: any
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS orders, count(*) AS rows
FROM stores s LEFT JOIN orders o ON o.store_id = s.id
WHERE o.total > 450
GROUP BY s.kind ORDER BY s.kind
