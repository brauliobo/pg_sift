-- trap: DISTINCT ON whose select list calls a volatile function: the plan keeps it where PostgreSQL evaluates it per row (a parallel plan evaluates it above the Gather, once per picked row)
-- shape: trap
-- expect: any
-- key: -
SELECT DISTINCT ON (tenant_id) tenant_id, id, 0 * random() AS z, total + 0 * random() AS t FROM orders WHERE tenant_id < 30 ORDER BY tenant_id, placed_at DESC, id
