-- trap: percentiles over NaN (sorts last), infinities, -0 and NULLs: the interpolation of PostgreSQL, including inf - inf
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, percentile_cont(0.5) WITHIN GROUP (ORDER BY f8) AS c50, percentile_cont(0.75) WITHIN GROUP (ORDER BY f8 DESC) AS c75,
       percentile_cont(1) WITHIN GROUP (ORDER BY f8) AS c100, percentile_disc(0.5) WITHIN GROUP (ORDER BY f8) AS d50, percentile_disc(0.2) WITHIN GROUP (ORDER BY f4) AS d20_f4
FROM t_float WHERE id <= 25
GROUP BY grp
ORDER BY grp
