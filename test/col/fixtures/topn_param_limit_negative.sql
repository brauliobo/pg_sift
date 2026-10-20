-- LIMIT $1 of a generic plan: a negative limit is the error of the Limit node
-- shape: topn_param
-- expect: any
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- error: yes
-- prepare: (bigint)
-- args: -1
SELECT id, tenant_id, total FROM orders ORDER BY total, id LIMIT $1
