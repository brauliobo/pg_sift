-- the image orders as the number: negative below zero below positive, the infinities at the ends, NaN above everything
-- shape: trap
-- expect: agg
-- key: -
SELECT 'gt1' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE nimg > 1 UNION ALL SELECT 'ge15', count(*), sum(qty) FROM t_wide WHERE nimg >= 1.5 UNION ALL SELECT 'lt_small', count(*), sum(qty) FROM t_wide WHERE nimg < -0.0001 UNION ALL SELECT 'le0', count(*), sum(qty) FROM t_wide WHERE nimg <= 0 UNION ALL SELECT 'nan', count(*), sum(qty) FROM t_wide WHERE nimg = 'NaN' UNION ALL SELECT 'gt_inf', count(*), sum(qty) FROM t_wide WHERE nimg > 'Infinity' UNION ALL SELECT 'ge_inf', count(*), sum(qty) FROM t_wide WHERE nimg >= 'Infinity' UNION ALL SELECT 'lt_ninf', count(*), sum(qty) FROM t_wide WHERE nimg < '-Infinity' UNION ALL SELECT 'le_ninf', count(*), sum(qty) FROM t_wide WHERE nimg <= '-Infinity' UNION ALL SELECT 'between', count(*), sum(qty) FROM t_wide WHERE nimg > -1000 AND nimg < 1000 ORDER BY 1
