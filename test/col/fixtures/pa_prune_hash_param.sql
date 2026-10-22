-- a generic plan prunes the hash remainders of a branch by a parameter: the scan computes the remainder of the value when it starts and drops the partitions that cannot hold it
-- shape: prepared
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (integer)
-- args: 4
SELECT grp, count(*) AS n, sum(amount) AS revenue FROM t_part WHERE id = $1 GROUP BY grp ORDER BY grp
