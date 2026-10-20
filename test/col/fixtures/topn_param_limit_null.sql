-- LIMIT $1 of a generic plan: NULL is all the rows, sorted
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint)
-- args: NULL
SELECT id, store_id, total FROM orders ORDER BY total, id LIMIT $1
