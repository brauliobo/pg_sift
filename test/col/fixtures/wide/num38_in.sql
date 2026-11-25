-- IN and NOT IN lists: a NaN or an unreachable element, a NULL element, an empty list
-- shape: trap
-- expect: agg
-- key: -
SELECT 'in' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE n38 IN (0, 1234.5, 'NaN', 1e60, 0.00000000005) UNION ALL SELECT 'in_null', count(*), sum(qty) FROM t_wide WHERE n38 IN (0, NULL) UNION ALL SELECT 'notin', count(*), sum(qty) FROM t_wide WHERE n38 NOT IN (0, 1234.5, 'NaN') UNION ALL SELECT 'notin_null', count(*), sum(qty) FROM t_wide WHERE n38 NOT IN (0, NULL) UNION ALL SELECT 'notin_far', count(*), sum(qty) FROM t_wide WHERE n38 NOT IN (1e60, 'Infinity') UNION ALL SELECT 'in_empty', count(*), sum(qty) FROM t_wide WHERE n38 = ANY (ARRAY[]::numeric[]) UNION ALL SELECT 'notin_empty', count(*), sum(qty) FROM t_wide WHERE n38 <> ALL (ARRAY[]::numeric[]) ORDER BY 1
