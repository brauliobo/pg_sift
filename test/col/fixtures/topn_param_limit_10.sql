-- LIMIT $1 of a generic plan: ten rows
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint)
-- args: 10
SELECT id, store_id, total FROM orders ORDER BY total, id LIMIT $1
