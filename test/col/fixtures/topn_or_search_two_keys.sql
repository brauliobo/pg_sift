-- an OR of searches, two sort keys
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, total FROM orders WHERE memo @@> 'express' OR memo @@> 'size' ORDER BY tenant_id DESC, total, id LIMIT 20
