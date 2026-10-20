-- arms whose matches overlap: a row matched by both is one row
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> 'clean' OR memo @@> 'clean wifi' ORDER BY placed_at DESC, id DESC LIMIT 10
