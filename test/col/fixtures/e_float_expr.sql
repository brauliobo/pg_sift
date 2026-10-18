-- float expressions PostgreSQL evaluates: roots, rounding through numeric, ceilings, a filter on a function of a float column; NaN, infinities and -0 among the values
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, sum(sqrt(abs(f8))) AS s, max(round(f8::numeric, 1)) AS m, min(ceil(f8)) AS c, count(*) FILTER (WHERE sqrt(abs(f8)) > 1) AS big, avg(f8 / 3) AS a
FROM t_float WHERE grp IN (1, 2, 3, 5, 7) GROUP BY grp ORDER BY grp
