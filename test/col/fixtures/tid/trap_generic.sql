-- trap: the bounds of a generic plan are parameters, not constants
-- shape: tid
-- expect: plain
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (tid, tid)
-- args: '(40,1)'::tid, '(350,1)'::tid
SELECT source, count(*) AS n, sum(tenant_id) AS s FROM orders WHERE ctid >= $1 AND ctid < $2 GROUP BY source ORDER BY source
