-- an OR of two searches of the index: the match set is their union
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> 'clean' OR memo @@> 'wifi' ORDER BY placed_at DESC, id DESC LIMIT 10
