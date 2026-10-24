-- a parameter of a generic plan added to a column
-- shape: topn_param
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (int)
-- args: 7
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY grp + $1, id LIMIT 12
