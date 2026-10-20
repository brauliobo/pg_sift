-- trap: groups beyond the hash memory: DISTINCT ON the primary key under a small work_mem keeps the plan
-- shape: trap
-- expect: plain
-- key: -
-- guc: work_mem = 64
-- guc: hash_mem_multiplier = 1
SELECT DISTINCT ON (id) id, tenant_id, total, status, placed_at, source FROM orders ORDER BY id
