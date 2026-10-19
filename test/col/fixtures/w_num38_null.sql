-- NULL and NaN tests: a NaN is not NULL
-- shape: trap
-- expect: agg
-- key: -
SELECT 'null' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE n38 IS NULL UNION ALL SELECT 'notnull', count(*), sum(qty) FROM t_wide WHERE n38 IS NOT NULL UNION ALL SELECT 'null19', count(*), sum(qty) FROM t_wide WHERE n19 IS NULL UNION ALL SELECT 'notnull19', count(*), sum(qty) FROM t_wide WHERE n19 IS NOT NULL ORDER BY 1
