-- trap: a generic plan prunes partitions when it runs (the values are not known when it is planned): the normal plan stays
-- shape: prepared
-- expect: plain
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (date, date)
-- args: DATE '2024-04-20', DATE '2024-08-01'
SELECT grp, count(*) AS n, sum(amount) AS revenue FROM t_part WHERE d >= $1 AND d < $2 GROUP BY grp ORDER BY grp
