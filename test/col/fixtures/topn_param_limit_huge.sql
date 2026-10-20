-- LIMIT $1 OFFSET $2 of a generic plan: limit and offset that overflow when added
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint, bigint)
-- args: 9223372036854775807, 10
SELECT id, store_id, total FROM orders ORDER BY total, id LIMIT $1 OFFSET $2
