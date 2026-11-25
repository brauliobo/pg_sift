-- an IN list that is a parameter array, and a float8 parameter, in a generic plan: PostgreSQL evaluates them
-- shape: prepared
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (numeric[], float8)
-- args: ARRAY[10.5, 7, 0.05], 3.5
SELECT grp, count(*) FILTER (WHERE n2 = ANY ($1)) AS hits, count(*) FILTER (WHERE n2 < $2) AS lt_f, count(*) FILTER (WHERE n2 <> ALL ($1)) AS misses
FROM t_num GROUP BY grp ORDER BY grp
