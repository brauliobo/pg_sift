-- an OR with an AND of two searches in a branch keeps the normal plan
-- shape: topn_or
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE (memo @@> 'cable' AND memo @@> 'wire') OR memo @@> 'express' ORDER BY placed_at DESC, id DESC LIMIT 10
