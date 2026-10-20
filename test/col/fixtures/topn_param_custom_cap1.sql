-- LIMIT $1 of a custom plan past the cap keeps the normal plan
-- shape: topn_param
-- expect: plain
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_custom_plan
-- prepare: (bigint)
-- args: 100001
SELECT id, tenant_id, total FROM orders ORDER BY total, id LIMIT $1
