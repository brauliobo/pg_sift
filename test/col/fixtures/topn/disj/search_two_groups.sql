-- two ORs of searches: the arms are the product of their branches
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE (memo @@> 'cable' OR memo @@> 'wire') AND (memo @@> 'express' OR memo @@> 'size') ORDER BY placed_at DESC, id DESC LIMIT 10
