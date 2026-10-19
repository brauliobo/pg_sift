-- every comparison of a numeric of two lanes with a constant, one query
-- shape: trap
-- expect: agg
-- key: -
SELECT 'gt' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE n38 > 0 UNION ALL SELECT 'ge', count(*), sum(qty) FROM t_wide WHERE n38 >= 0 UNION ALL SELECT 'lt', count(*), sum(qty) FROM t_wide WHERE n38 < 0 UNION ALL SELECT 'le', count(*), sum(qty) FROM t_wide WHERE n38 <= 0 UNION ALL SELECT 'eq', count(*), sum(qty) FROM t_wide WHERE n38 = 0 UNION ALL SELECT 'ne', count(*), sum(qty) FROM t_wide WHERE n38 <> 0 ORDER BY 1
