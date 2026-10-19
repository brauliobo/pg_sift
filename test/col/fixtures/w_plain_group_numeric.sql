-- a key of a numeric of two lanes: the values are read, the normal plan groups them
-- shape: expr
-- expect: plain
-- key: 1
SELECT n19, count(*) AS n FROM t_wide WHERE grp = 6 GROUP BY n19 ORDER BY n19
