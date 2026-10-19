-- equal numbers of other scales are one value: 1, 1.0, 1.00 and 1.000 all pass x = 1, a NaN is equal to itself
-- shape: trap
-- expect: agg
-- key: -
SELECT 'eq1' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE nimg = 1 UNION ALL SELECT 'eq1_scale', count(*), sum(qty) FROM t_wide WHERE nimg = 1.0000 UNION ALL SELECT 'ne1', count(*), sum(qty) FROM t_wide WHERE nimg <> 1 UNION ALL SELECT 'eq15', count(*), sum(qty) FROM t_wide WHERE nimg = 1.50 UNION ALL SELECT 'eq100', count(*), sum(qty) FROM t_wide WHERE nimg = 100 UNION ALL SELECT 'zero', count(*), sum(qty) FROM t_wide WHERE nimg = 0.000 ORDER BY 1
