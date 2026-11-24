-- an arm with no positive term matches every row without the term
-- shape: topn_or
-- expect: any
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE memo @@> 'NOT wire' OR memo @@> 'cable' ORDER BY placed_at DESC, id DESC LIMIT 10
