-- an arm that is a phrase: the index leaves rows to their text
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE memo @@> '"cable wire"' OR memo @@> 'express' ORDER BY placed_at DESC, id DESC LIMIT 10
