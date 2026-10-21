-- arms with a NOT inside: each keeps its positive term
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE memo @@> 'cable NOT wire' OR memo @@> 'express NOT cable' ORDER BY placed_at DESC, id DESC LIMIT 10
