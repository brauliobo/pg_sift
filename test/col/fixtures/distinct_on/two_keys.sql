-- DISTINCT ON two columns, listed in another order than the ORDER BY has them
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (source, tenant_id) tenant_id, source, id, placed_at FROM orders WHERE tenant_id < 25 ORDER BY tenant_id, source, placed_at DESC, id
