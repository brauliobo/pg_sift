-- numeric functions and casts as aggregate arguments: power, sqrt, ln, modulo, a numeric(p, s) cast that rounds (and would raise for the rows the FILTER leaves out), numeric to float8
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, max(power(n2, 2)) AS p, max(sqrt(abs(n2))) AS r, sum(n6::float8) AS f6, avg(n6::float8 / 3) AS f6d, max(n2::numeric(8, 1)) FILTER (WHERE n2 BETWEEN -50 AND 120) AS rescaled,
       min(ln(abs(n2) + 1)) AS l, max(n2 % 3) AS m, sum(trunc(n6 / 7, 2)) AS t, count(DISTINCT n2 % 5) AS d
FROM t_num GROUP BY grp ORDER BY grp
