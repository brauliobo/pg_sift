-- interval IN, NOT IN and NULL tests
-- shape: trap
-- expect: agg
-- key: -
SELECT 'in' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE iv IN (interval '1 month', interval '1 year', interval '-1 microsecond') UNION ALL SELECT 'notin', count(*), sum(qty) FROM t_wide WHERE iv NOT IN (interval '0', interval '1 day') UNION ALL SELECT 'notin_null', count(*), sum(qty) FROM t_wide WHERE iv NOT IN (interval '0', NULL) UNION ALL SELECT 'null', count(*), sum(qty) FROM t_wide WHERE iv IS NULL ORDER BY 1
