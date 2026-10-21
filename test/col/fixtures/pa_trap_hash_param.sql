-- trap: a generic plan prunes the hash remainders of a branch by a parameter, which a partition bound cannot refute: the normal plan stays
-- shape: prepared
-- expect: plain
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (integer)
-- args: 4
SELECT grp, count(*) AS n, sum(amount) AS revenue FROM t_part WHERE id = $1 GROUP BY grp ORDER BY grp
