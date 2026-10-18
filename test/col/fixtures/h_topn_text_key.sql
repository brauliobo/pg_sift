-- a text sort key keeps the normal plan
-- shape: trap
-- expect: plain
-- key: -
SELECT id, status FROM orders WHERE memo @@> 'gift' ORDER BY status, id LIMIT 10
