-- NaN as a parameter: only a NaN is greater than or equal to it
-- shape: prepared
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (numeric)
-- args: 'NaN'
SELECT count(*) AS n, sum(qty) AS q FROM t_wide WHERE n38 >= $1
