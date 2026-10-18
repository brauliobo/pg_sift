-- trap: NaN, infinities, -0 and 0 as group keys (-0 and 0 are one group)
-- shape: trap
-- expect: any
-- key: -
SELECT f8, count(*) AS n FROM t_float WHERE grp BETWEEN 2 AND 5 GROUP BY f8 ORDER BY f8
