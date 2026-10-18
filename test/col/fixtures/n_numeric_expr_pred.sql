-- numeric predicates on expressions: arithmetic against constants (inexact ones too), a comparison of two columns, round/floor in a filter, NULL operands
-- shape: S6
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, count(*) AS n,
       count(*) FILTER (WHERE n2 * 2 > 20) AS dbl_gt, count(*) FILTER (WHERE n2 * 2 = 21) AS dbl_eq, count(*) FILTER (WHERE n2 + n6 < 0.5) AS add_lt,
       count(*) FILTER (WHERE n2 * grp >= 14.005) AS int_ge, count(*) FILTER (WHERE n2 > n6) AS col_gt, count(*) FILTER (WHERE n2 = n0) AS col_eq,
       count(*) FILTER (WHERE n0 <> n6 * 2) AS col_ne, count(*) FILTER (WHERE round(n2, 0) = 11) AS round_eq, count(*) FILTER (WHERE floor(n6) < 0) AS floor_lt,
       count(*) FILTER (WHERE abs(n2) <= 3.25) AS abs_le, count(*) FILTER (WHERE 20 < n2 * 2) AS commuted, count(*) FILTER (WHERE n2 * n6 > 'NaN'::numeric) AS nan_gt,
       count(*) FILTER (WHERE n2 * n6 < 'Infinity'::numeric) AS inf_lt
FROM t_num
WHERE n2 - n6 > -1000 OR n2 IS NULL
GROUP BY grp
ORDER BY grp
