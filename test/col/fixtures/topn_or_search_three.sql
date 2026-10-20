-- an OR of three searches
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE memo @@> 'clean' OR memo @@> 'wire' OR memo @@> 'express' ORDER BY placed_at, id LIMIT 15
