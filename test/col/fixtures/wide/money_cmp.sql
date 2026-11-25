-- money is the integer of the smallest unit: every comparison, both ends of the range, ties
-- shape: trap
-- expect: agg
-- key: -
SELECT 'gt' AS op, count(*) AS n, sum(qty) AS q FROM t_wide WHERE m > '123.45' UNION ALL SELECT 'le', count(*), sum(qty) FROM t_wide WHERE m <= '-123.45' UNION ALL SELECT 'eq', count(*), sum(qty) FROM t_wide WHERE m = '1.00' UNION ALL SELECT 'ne', count(*), sum(qty) FROM t_wide WHERE m <> '1.00' UNION ALL SELECT 'max', count(*), sum(qty) FROM t_wide WHERE m = '92233720368547758.07' UNION ALL SELECT 'min', count(*), sum(qty) FROM t_wide WHERE m = '-92233720368547758.08' UNION ALL SELECT 'around0', count(*), sum(qty) FROM t_wide WHERE m > '-5.00' AND m < '5.00' AND m <> '0' ORDER BY 1
