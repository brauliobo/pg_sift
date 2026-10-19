-- equal intervals spelled differently (1 month, 30 days, 720 hours) are one value, 1 mon 1 day is 31 days
-- shape: trap
-- expect: agg
-- key: -
SELECT 'month' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE iv = interval '1 month' UNION ALL SELECT 'days', count(*), sum(qty) FROM t_wide WHERE iv = interval '30 days' UNION ALL SELECT 'hours', count(*), sum(qty) FROM t_wide WHERE iv = interval '720 hours' UNION ALL SELECT 'd31', count(*), sum(qty) FROM t_wide WHERE iv = interval '1 mon 1 day' UNION ALL SELECT 'year', count(*), sum(qty) FROM t_wide WHERE iv = interval '12 mons' UNION ALL SELECT 'micro', count(*), sum(qty) FROM t_wide WHERE iv = interval '1 microsecond' ORDER BY 1
