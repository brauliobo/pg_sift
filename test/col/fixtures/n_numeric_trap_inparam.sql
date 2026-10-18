-- trap: an IN list that is a parameter array is not a list of constants the scan can scale when it starts (the normal plan, same rows)
-- shape: prepared
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (numeric[])
-- args: ARRAY[10.5, 7, 0.05]
SELECT grp, count(*) FILTER (WHERE n2 = ANY ($1)) AS hits FROM t_num GROUP BY grp ORDER BY grp
