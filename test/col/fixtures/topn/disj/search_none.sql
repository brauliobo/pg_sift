-- one arm matches nothing
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE memo @@> 'zzzzzz' OR memo @@> 'wire' ORDER BY placed_at DESC, id DESC LIMIT 10
