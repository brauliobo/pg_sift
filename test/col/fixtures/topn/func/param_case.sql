-- a parameter in a CASE
-- shape: topn_param
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (int)
-- args: 3
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY CASE WHEN grp > $1 THEN 1 ELSE 0 END, id DESC LIMIT 12
