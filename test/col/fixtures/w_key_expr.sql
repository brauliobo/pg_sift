-- keys that are expressions of wide leaves
-- shape: expr
-- expect: agg
-- key: -
SELECT n38 > 0 AS positive, iv >= interval '1 month' AS month_or_more, count(*) AS n FROM t_wide GROUP BY 1, 2 ORDER BY 1, 2
