-- DISTINCT ON over no rows
-- shape: distinct_on
-- expect: agg
-- key: -
SELECT DISTINCT ON (tenant_id) tenant_id, id FROM orders WHERE tenant_id < 0 ORDER BY tenant_id, id
