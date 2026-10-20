-- parameters of a generic plan: numeric, money and interval operands, evaluated when the scan starts
-- shape: prepared
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (numeric, money, interval)
-- args: 1234.5, '-123.45', '1 month'
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_wide WHERE n38 > $1 AND m > $2 AND iv <> $3 GROUP BY grp ORDER BY grp
