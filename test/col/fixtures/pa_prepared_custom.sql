-- a prepared statement planned with its values (a custom plan): the pruning happens when it is planned
-- shape: prepared
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_custom_plan
-- prepare: (date, date)
-- args: DATE '2024-04-20', DATE '2024-08-01'
SELECT grp, count(*) AS n, sum(amount) AS revenue FROM t_part WHERE d >= $1 AND d < $2 GROUP BY grp ORDER BY grp
