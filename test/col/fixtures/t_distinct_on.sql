-- trap: DISTINCT ON (no aggregate)
-- shape: trap
-- expect: plain
-- key: -
SELECT DISTINCT ON (tenant_id) tenant_id, id, placed_at FROM orders WHERE tenant_id < 50 ORDER BY tenant_id, placed_at DESC, id
