-- an OR next to a plain match keeps the normal plan
-- shape: topn_or
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> 'express' AND (memo @@> 'clean' OR memo @@> 'wifi') ORDER BY placed_at DESC, id DESC LIMIT 10
