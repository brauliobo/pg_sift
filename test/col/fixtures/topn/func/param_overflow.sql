-- a parameter that overflows the sum (22003)
-- shape: topn_param
-- expect: agg
-- error: yes
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (int)
-- args: 2147483647
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY i4 + $1, id LIMIT 12
