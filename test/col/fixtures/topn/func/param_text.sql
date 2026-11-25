-- a text parameter appended to a column
-- shape: topn_param
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (text)
-- args: '-x'
-- scenario: yes
SELECT id, t FROM t_text ORDER BY t || $1, id LIMIT 12
