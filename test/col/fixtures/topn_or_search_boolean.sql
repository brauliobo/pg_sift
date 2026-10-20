-- arms with their own OR, AND and groups
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> '(clean OR wifi) AND NOT express' OR memo @@> 'size color' ORDER BY placed_at DESC, id DESC LIMIT 10
