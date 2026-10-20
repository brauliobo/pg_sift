-- the order of intervals, the infinities and the largest and smallest values
-- shape: trap
-- expect: agg
-- key: -
SELECT 'gt' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE iv > interval '1 month' UNION ALL SELECT 'ge', count(*), sum(qty) FROM t_wide WHERE iv >= interval '1 month' UNION ALL SELECT 'lt', count(*), sum(qty) FROM t_wide WHERE iv < interval '-1 month' UNION ALL SELECT 'le0', count(*), sum(qty) FROM t_wide WHERE iv <= interval '0' UNION ALL SELECT 'ne0', count(*), sum(qty) FROM t_wide WHERE iv <> interval '0' UNION ALL SELECT 'inf', count(*), sum(qty) FROM t_wide WHERE iv = 'infinity' UNION ALL SELECT 'ninf', count(*), sum(qty) FROM t_wide WHERE iv = '-infinity' UNION ALL SELECT 'near_max', count(*), sum(qty) FROM t_wide WHERE iv > interval '2147483647 months 2147483647 days 9223372036854775806 us' UNION ALL SELECT 'between', count(*), sum(qty) FROM t_wide WHERE iv > interval '59 days' AND iv <= interval '2 mons' ORDER BY 1
