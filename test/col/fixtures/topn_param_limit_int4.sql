-- LIMIT $1 of a generic plan: an int4 parameter (the limit is int8)
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (int)
-- args: 10
SELECT id, tenant_id, total FROM orders ORDER BY total, id LIMIT $1
