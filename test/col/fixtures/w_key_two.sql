-- a wide key with another leaf
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, n19, count(*) AS n FROM t_wide WHERE grp < 4 GROUP BY grp, n19 ORDER BY grp, n19
