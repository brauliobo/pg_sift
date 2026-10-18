-- trap: a numeric expression, a cast and a numeric filter are not the stored value: the normal plan (same rows)
-- shape: trap
-- expect: plain
-- key: -
SELECT grp, sum(n2 * 2) AS dbl, sum(n2::float8) AS f, sum(n0) FILTER (WHERE n2 > 0) AS pos
FROM t_num GROUP BY grp ORDER BY grp
