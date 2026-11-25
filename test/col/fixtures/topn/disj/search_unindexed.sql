-- a branch that is a match of a column with no index keeps the normal plan
-- shape: topn_or
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, tenant_id, placed_at FROM orders WHERE memo @@> 'cable' OR lang @@> 'en' ORDER BY placed_at DESC, id DESC LIMIT 10
