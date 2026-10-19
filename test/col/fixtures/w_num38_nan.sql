-- NaN as a stored value and as a constant: greater than every number, equal to itself
-- shape: trap
-- expect: agg
-- key: -
SELECT 'eq' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE n38 = 'NaN' UNION ALL SELECT 'ne', count(*), sum(qty) FROM t_wide WHERE n38 <> 'NaN' UNION ALL SELECT 'lt', count(*), sum(qty) FROM t_wide WHERE n38 < 'NaN' UNION ALL SELECT 'le', count(*), sum(qty) FROM t_wide WHERE n38 <= 'NaN' UNION ALL SELECT 'gt', count(*), sum(qty) FROM t_wide WHERE n38 > 'NaN' UNION ALL SELECT 'ge', count(*), sum(qty) FROM t_wide WHERE n38 >= 'NaN' UNION ALL SELECT 'gt_pinf', count(*), sum(qty) FROM t_wide WHERE n38 > 'Infinity' UNION ALL SELECT 'lt_pinf', count(*), sum(qty) FROM t_wide WHERE n38 < 'Infinity' UNION ALL SELECT 'gt_ninf', count(*), sum(qty) FROM t_wide WHERE n38 > '-Infinity' UNION ALL SELECT 'lt_ninf', count(*), sum(qty) FROM t_wide WHERE n38 < '-Infinity' ORDER BY 1
