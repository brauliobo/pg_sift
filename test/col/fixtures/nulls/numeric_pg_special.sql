-- NaN and Infinity inside numeric arithmetic: PostgreSQL's numeric functions give the NaN and the infinities
-- shape: expr
-- expect: agg
-- key: -
SELECT grp, max(n2 + 'NaN'::numeric) AS nan, min(n2 * 'Infinity'::numeric) AS inf, sum(n2 + '-Infinity'::numeric) AS ninf, count(*) FILTER (WHERE n2 < 'NaN'::numeric + 1) AS lt_nan
FROM t_num WHERE n2 > 0 GROUP BY grp ORDER BY grp
