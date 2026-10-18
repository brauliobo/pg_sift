-- two keys of opposite directions
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, store_id, total FROM orders WHERE memo @@> 'express' ORDER BY store_id DESC, total, id LIMIT 20
