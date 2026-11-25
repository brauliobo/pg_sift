-- trap: a float8 sum of many inexact values: the order of the additions moves the last digits
-- shape: trap
-- expect: any
-- key: -
SELECT count(*) AS n, sum(f8) AS s, avg(f8) AS a FROM t_float WHERE grp = 7
