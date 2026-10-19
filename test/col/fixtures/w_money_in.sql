-- money IN, NOT IN and NULL tests
-- shape: trap
-- expect: agg
-- key: -
SELECT 'in' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE m IN ('0', '1.00', '-1.00') UNION ALL SELECT 'notin', count(*), sum(qty) FROM t_wide WHERE m NOT IN ('0', '1.00') UNION ALL SELECT 'in_null', count(*), sum(qty) FROM t_wide WHERE m IN ('1.00', NULL) UNION ALL SELECT 'notin_null', count(*), sum(qty) FROM t_wide WHERE m NOT IN ('1.00', NULL) UNION ALL SELECT 'null', count(*), sum(qty) FROM t_wide WHERE m IS NULL ORDER BY 1
