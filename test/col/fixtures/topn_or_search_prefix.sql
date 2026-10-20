-- arms with a prefix and a fuzzy term
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> 'exch*' OR memo @@> 'wifii~1' ORDER BY placed_at DESC, id DESC LIMIT 10
