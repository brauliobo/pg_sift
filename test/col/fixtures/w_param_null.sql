-- NULL parameters compare as unknown: no row passes
-- shape: prepared
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (numeric, money, interval)
-- args: NULL, NULL, NULL
SELECT count(*) AS n FROM t_wide WHERE n38 > $1 AND m > $2 AND iv = $3
