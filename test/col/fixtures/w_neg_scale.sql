-- numeric(5, -2): the values are multiples of 100, a constant between two of them equals none
-- shape: trap
-- expect: agg
-- key: -
SELECT 'eq' AS op, count(*) AS n FROM t_wide WHERE nneg = 100 UNION ALL SELECT 'eq_between', count(*) FROM t_wide WHERE nneg = 150 UNION ALL SELECT 'gt', count(*) FROM t_wide WHERE nneg > -5000 UNION ALL SELECT 'in', count(*) FROM t_wide WHERE nneg IN (100, -100, 12345) UNION ALL SELECT 'ge_top', count(*) FROM t_wide WHERE nneg >= 99999 ORDER BY 1
