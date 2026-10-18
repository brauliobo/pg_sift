-- numeric(p, s) sums, averages, minima and maxima: the scale of the result, the division of avg, NULL-only groups, negative values and sums past int64
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, count(n2) AS c, sum(n2) AS s2, avg(n2) AS a2, min(n2) AS lo2, max(n2) AS hi2, sum(n0) AS s0, avg(n0) AS a0, min(n0) AS lo0, max(n0) AS hi0,
       sum(n6) AS s6, avg(n6) AS a6, min(n6) AS lo6, max(n6) AS hi6
FROM t_num GROUP BY grp ORDER BY grp
