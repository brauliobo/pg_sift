-- FETCH FIRST WITH TIES keeps the normal plan
-- shape: topn_param
-- expect: plain
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint)
-- args: 10
SELECT id, store_id, total FROM orders ORDER BY total, id FETCH FIRST $1 ROWS WITH TIES
