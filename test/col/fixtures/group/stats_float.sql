-- generic aggregates over float columns: sum(float4) adds in float4 as PostgreSQL does, the regression family takes float8 casts of the columns
-- shape: flat
-- expect: agg
-- key: -
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
SELECT grp, sum(f4) AS s4, stddev_samp(f8) AS sd, var_pop(f4) AS vp, corr(f8, f4) AS c, covar_samp(f8, f4) AS cov, regr_slope(f8, f4) AS slope,
       regr_count(f8, f4) AS n, coldiff.sum_sq(f8) AS sq
FROM t_float WHERE grp IN (1, 7) GROUP BY grp ORDER BY grp
