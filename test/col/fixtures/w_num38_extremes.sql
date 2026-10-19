-- the largest and smallest values of numeric(38, 10) and the 64-bit boundaries inside the lanes
-- shape: trap
-- expect: agg
-- key: -
SELECT 'max' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE n38 = 9999999999999999999999999999.9999999999 UNION ALL SELECT 'min', count(*), sum(qty) FROM t_wide WHERE n38 = -9999999999999999999999999999.9999999999 UNION ALL SELECT 'near_max', count(*), sum(qty) FROM t_wide WHERE n38 > 9999999999999999999999999999.9999999998 UNION ALL SELECT 'near_min', count(*), sum(qty) FROM t_wide WHERE n38 < -9999999999999999999999999999.9999999998 UNION ALL SELECT 'u64', count(*), sum(qty) FROM t_wide WHERE n38 >= 18446744073709551615 UNION ALL SELECT 'i64', count(*), sum(qty) FROM t_wide WHERE n38 BETWEEN -9223372036854775809 AND 9223372036854775808 ORDER BY 1
