-- a sub-select with no row: nothing to read
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, total FROM orders WHERE store_id IN (SELECT id FROM stores WHERE kind = 'no such kind') ORDER BY total DESC, id LIMIT 5
