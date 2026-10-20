-- DISTINCT ON over no rows
-- shape: distinct_on
-- expect: agg
-- key: -
SELECT DISTINCT ON (store_id) store_id, id FROM orders WHERE store_id < 0 ORDER BY store_id, id
