-- coalesce of an int column under a filter, then a second key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, customer_id, store_id FROM orders WHERE status = 'shipped' ORDER BY coalesce(customer_id, 2147483647) DESC, store_id, id LIMIT 12
