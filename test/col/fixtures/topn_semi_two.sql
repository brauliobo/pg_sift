-- two sub-selects, one with duplicate keys (a semi join matches a row once)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, total FROM orders WHERE store_id IN (SELECT id FROM stores WHERE id % 3 = 0) AND store_id IN (SELECT store_id FROM orders_odd WHERE total > 300) ORDER BY total, id LIMIT 12
