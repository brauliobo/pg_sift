-- numeric aggregate arguments PostgreSQL evaluates: division (its own scale and rounding), products past 38 digits and sums past 26, a FILTER that keeps the division off a zero divisor
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, sum(n2 / 3) AS s_div, avg(n6 / n2) FILTER (WHERE n2 <> 0) AS a_ratio, min(n2 / n6) FILTER (WHERE n6 > 0) AS lo, count(n2 / 3) AS c,
       max(n2 * n2 * n2 * n2) AS wide, max(floor(n6 * n6 * n6)) AS f, sum(n2 * n6) AS big, sum(n0 * n0 * 1.5) AS s0
FROM t_num GROUP BY grp ORDER BY grp
