-- LIMIT of an expression of a parameter
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (int)
-- args: 4
SELECT id, store_id, total FROM orders ORDER BY total, id LIMIT $1 * 3 + 1
