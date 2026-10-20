-- two ORs of searches keep the normal plan
-- shape: topn_or
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE (memo @@> 'clean' OR memo @@> 'wifi') AND (memo @@> 'express' OR memo @@> 'size') ORDER BY placed_at DESC, id DESC LIMIT 10
