-- trap: FOR UPDATE (locks heap rows; no aggregate)
-- shape: trap
-- expect: plain
-- wrap: subquery
-- key: -
SELECT id, tenant_id FROM orders WHERE tenant_id < 5 ORDER BY id LIMIT 20 FOR UPDATE
