-- two keys of opposite directions
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, tenant_id, total FROM orders WHERE memo @@> 'express' ORDER BY tenant_id DESC, total, id LIMIT 20
