-- an arm with no positive term is the error of the normal plan
-- shape: topn_or
-- expect: any
-- key: -
-- scenario: yes
-- error: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> 'NOT wifi' OR memo @@> 'clean' ORDER BY placed_at DESC, id DESC LIMIT 10
