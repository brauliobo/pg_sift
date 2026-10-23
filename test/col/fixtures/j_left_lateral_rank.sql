-- the ranking form: per order the average of its items (an order with none keeps its row), then the average over the orders of a store
-- shape: S7
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.store_id, count(s.id) AS orders, avg(s.total) AS total, avg(s.score) AS score
FROM (SELECT o.id, o.store_id, max(o.total) AS total, avg(i.price * i.qty) AS score
      FROM orders o LEFT JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8) ON true
      WHERE o.deleted_at IS NULL AND o.store_id < 300
      GROUP BY o.id, o.store_id) s
GROUP BY s.store_id ORDER BY s.store_id
