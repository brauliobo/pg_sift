-- a Top-N of a table partitioned by range, as the second key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, total FROM orders_part ORDER BY total DESC, id LIMIT 20
