-- trap: DISTINCT ON (no aggregate)
-- shape: trap
-- expect: plain
-- key: -
SELECT DISTINCT ON (store_id) store_id, id, placed_at FROM orders WHERE store_id < 50 ORDER BY store_id, placed_at DESC, id
