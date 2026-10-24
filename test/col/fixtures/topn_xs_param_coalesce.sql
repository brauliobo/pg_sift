-- a parameter as the default of a coalesce
-- shape: topn_param
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (int)
-- args: 250
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY coalesce(i4, $1), id LIMIT 12
