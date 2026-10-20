-- LIMIT $1 of a generic plan: one row
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint)
-- args: 1
SELECT id, tenant_id, total FROM orders ORDER BY total, id LIMIT $1
