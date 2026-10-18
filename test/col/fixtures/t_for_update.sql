-- trap: FOR UPDATE (locks heap rows; no aggregate)
-- shape: trap
-- expect: plain
-- wrap: subquery
-- key: -
SELECT id, store_id FROM orders WHERE store_id < 5 ORDER BY id LIMIT 20 FOR UPDATE
