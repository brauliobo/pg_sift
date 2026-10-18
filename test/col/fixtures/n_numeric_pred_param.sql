-- numeric parameters of a generic plan: the constants are scaled when the scan starts, a NaN and an infinite one included
-- shape: prepared
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- prepare: (numeric, numeric, numeric)
-- args: 10.505, 'NaN', -0.0049
SELECT grp, count(*) FILTER (WHERE n2 < $1) AS lt, count(*) FILTER (WHERE n2 = $1) AS eq, count(*) FILTER (WHERE n2 <> $2) AS ne_nan, count(*) FILTER (WHERE n2 >= $2) AS ge_nan,
       count(*) FILTER (WHERE n2 > $3) AS gt, sum(n2) FILTER (WHERE n2 <= $3) AS le_sum
FROM t_num GROUP BY grp ORDER BY grp
