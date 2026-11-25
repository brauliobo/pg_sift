-- a sub-select with no row: nothing to read
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, total FROM orders WHERE tenant_id IN (SELECT id FROM tenants WHERE kind = 'no such kind') ORDER BY total DESC, id LIMIT 5
