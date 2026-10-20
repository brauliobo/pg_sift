-- a key of a numeric of two lanes
-- shape: expr
-- expect: agg
-- key: -
SELECT n19, count(*) AS n FROM t_wide WHERE grp = 6 GROUP BY n19 ORDER BY n19
