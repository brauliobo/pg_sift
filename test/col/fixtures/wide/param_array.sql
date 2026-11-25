-- an array parameter: an IN list evaluated when the scan starts, with a NULL and a NaN element
-- shape: prepared
-- expect: agg
-- key: -
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (numeric[], numeric[])
-- args: ARRAY[1, 1.5, 'NaN', NULL]::numeric[], ARRAY[0, 'NaN']::numeric[]
SELECT grp, count(*) AS n FROM t_wide WHERE nimg = ANY ($1) AND n38 <> ALL ($2) GROUP BY grp ORDER BY grp
