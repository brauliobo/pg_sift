-- numeric(40, 5): one digit more than a lane pair holds
-- shape: trap
-- expect: agg
-- key: -
SELECT 'ge' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE nhuge >= 100000000000000000000000000000000.5 UNION ALL SELECT 'lt', count(*), sum(qty) FROM t_wide WHERE nhuge < -99999999999999999999999999999999999.99999 UNION ALL SELECT 'max', count(*), sum(qty) FROM t_wide WHERE nhuge = 99999999999999999999999999999999999.99999 UNION ALL SELECT 'nan', count(*), sum(qty) FROM t_wide WHERE nhuge = 'NaN' ORDER BY 1
