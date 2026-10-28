-- count(DISTINCT) of numeric columns and expressions: equal by value at one scale, NULL left out, per group and over everything, with FILTER
-- shape: notes-path-exists
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(DISTINCT n2) AS d2, count(DISTINCT n0) AS d0, count(DISTINCT n6) AS d6, count(DISTINCT n2 * 10) AS d_x10, count(DISTINCT round(n2, 0)) AS d_round,
       count(DISTINCT n2 + 0.10) AS d_scale, count(DISTINCT n2) FILTER (WHERE n2 > 0) AS d_pos, count(DISTINCT abs(n2)) AS d_abs,
       sum(DISTINCT n2) AS sd2, avg(DISTINCT n0) AS ad0, sum(DISTINCT round(n2, 0)) AS sd_round, sum(DISTINCT n6) FILTER (WHERE n6 > 0) AS sd6_pos
FROM t_num
GROUP BY grp
ORDER BY grp
