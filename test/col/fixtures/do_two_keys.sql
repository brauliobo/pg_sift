-- DISTINCT ON two columns, listed in another order than the ORDER BY has them
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (source, store_id) store_id, source, id, placed_at FROM orders WHERE store_id < 25 ORDER BY store_id, source, placed_at DESC, id
