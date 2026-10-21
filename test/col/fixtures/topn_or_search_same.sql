-- arms with the same query
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE memo @@> 'cable' OR memo @@> 'cable' ORDER BY placed_at DESC, id DESC LIMIT 10
