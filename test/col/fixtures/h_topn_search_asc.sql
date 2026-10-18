-- ascending, with column filters on top of the match set and an offset
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, placed_at, total FROM orders WHERE memo @@> 'clean wifi' AND store_id < 300 AND source <> 'web' ORDER BY placed_at, id LIMIT 8 OFFSET 3
