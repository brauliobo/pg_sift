-- trap: sum(int8) is a numeric past 2^63, avg(int8) a numeric
-- shape: trap
-- expect: any
-- key: -
SELECT grp, sum(i8) AS s, avg(i8) AS a, min(i8) AS lo, max(i8) AS hi FROM t_int GROUP BY grp ORDER BY grp
