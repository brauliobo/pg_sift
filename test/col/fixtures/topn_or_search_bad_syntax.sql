-- an arm that is not a query is the error of the normal plan
-- shape: topn_or
-- expect: any
-- key: -
-- scenario: yes
-- error: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> 'cable)' OR memo @@> 'wire' ORDER BY placed_at DESC, id DESC LIMIT 10
