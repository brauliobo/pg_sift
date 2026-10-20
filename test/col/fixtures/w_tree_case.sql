-- CASE on wide conditions
-- shape: expr
-- expect: agg
-- key: -
SELECT CASE WHEN n38 > 1e3 THEN 'a' WHEN iv > interval '1 month' THEN 'b' WHEN m IS NULL THEN 'c' ELSE 'd' END AS k, count(*) AS n FROM t_wide GROUP BY 1 ORDER BY 1
