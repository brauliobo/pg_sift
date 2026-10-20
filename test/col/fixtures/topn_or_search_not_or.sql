-- a NOT over an OR keeps the normal plan
-- shape: topn_or
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE NOT (memo @@> 'clean' OR memo @@> 'wifi') ORDER BY placed_at DESC, id DESC LIMIT 10
