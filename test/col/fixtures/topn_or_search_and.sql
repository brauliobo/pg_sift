-- an OR next to a plain match: the arms are the plain match with each branch
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE memo @@> 'express' AND (memo @@> 'cable' OR memo @@> 'wire') ORDER BY placed_at DESC, id DESC LIMIT 10
