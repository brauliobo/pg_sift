-- a NOT over an OR keeps the normal plan
-- shape: topn_or
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE NOT (memo @@> 'cable' OR memo @@> 'wire') ORDER BY placed_at DESC, id DESC LIMIT 10
