-- numeric(10,2) sums, averages and extremes with NULLs
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, sum(amount) AS total, avg(amount) AS mean, min(amount) AS lo, max(amount) AS hi, count(amount) AS n FROM t_part GROUP BY grp ORDER BY grp
