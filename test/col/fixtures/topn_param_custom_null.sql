-- LIMIT $1 NULL of a custom plan keeps the normal plan
-- shape: topn_param
-- expect: plain
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_custom_plan
-- prepare: (bigint)
-- args: NULL
SELECT id, store_id, total FROM orders ORDER BY total, id LIMIT $1
