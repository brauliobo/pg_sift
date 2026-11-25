-- NaN stored in a numeric column, read by the kernel shapes: PostgreSQL's order (above every number, equal to itself) in extremes, sums, counts, DISTINCT and the numeric programs
-- shape: expr
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, min(n2) AS lo, max(n2) AS hi, sum(n2) AS s, avg(n6) AS a, count(n2) AS c, count(DISTINCT n2) AS d, sum(n2 + n6) AS sp, max(n2 * 2) AS m2, min(floor(n6)) AS fl
FROM t_nan GROUP BY grp ORDER BY grp
