-- OFFSET $1 with no limit keeps the normal plan
-- shape: topn_param
-- expect: plain
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint)
-- args: 10
SELECT id, tenant_id, total FROM orders ORDER BY total, id OFFSET $1
