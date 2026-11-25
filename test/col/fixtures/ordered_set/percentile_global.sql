-- ordered sets without a group, over an empty set and over a set of NULLs only
-- shape: whole-percentiles-by-category
-- expect: agg
-- key: -
SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY f8) AS c, percentile_disc(0.5) WITHIN GROUP (ORDER BY f4) AS d,
       (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY f8) FROM t_float WHERE id = 6) AS only_null,
       (SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY f8) FROM t_float WHERE id = 0) AS empty
FROM t_float WHERE grp = 1
