-- two arms that are phrases
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> '"clean wifi"' OR memo @@> '"size color"' ORDER BY placed_at DESC, id DESC LIMIT 10
