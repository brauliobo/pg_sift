-- trap: NaN and infinities in sums, averages, min and max, with NULLs
-- shape: trap
-- expect: any
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, count(f8) AS c, sum(f8) AS s, avg(f8) AS a, min(f8) AS lo, max(f8) AS hi FROM t_float WHERE id <= 20 GROUP BY grp ORDER BY grp
