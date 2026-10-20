-- IN and NOT IN on the image: ties between scales, NaN and Infinity as elements
-- shape: trap
-- expect: agg
-- key: -
SELECT 'in' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE nimg IN (1, 1.5, 'NaN', 'Infinity', 0.0) UNION ALL SELECT 'notin', count(*), sum(qty) FROM t_wide WHERE nimg NOT IN (1, 1.5, 'NaN') UNION ALL SELECT 'in_null', count(*), sum(qty) FROM t_wide WHERE nimg IN (1, NULL) UNION ALL SELECT 'notin_null', count(*), sum(qty) FROM t_wide WHERE nimg NOT IN (1, NULL) UNION ALL SELECT 'null', count(*), sum(qty) FROM t_wide WHERE nimg IS NULL UNION ALL SELECT 'notnull', count(*), sum(qty) FROM t_wide WHERE nimg IS NOT NULL ORDER BY 1
