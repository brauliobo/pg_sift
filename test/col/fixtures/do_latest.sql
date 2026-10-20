-- DISTINCT ON: the latest order of each store (the ORDER BY ends in the primary key, so the row is determined)
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (store_id) store_id, id, placed_at, total FROM orders WHERE store_id < 60 ORDER BY store_id, placed_at DESC, id
