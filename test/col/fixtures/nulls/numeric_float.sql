-- numeric to float8: a numeric(12, 2) column cast is exact in double (below 2^53, divided by a power of ten), also inside float8 arithmetic
-- shape: notes-path-exists
-- expect: agg
-- key: -
SELECT grp, sum(n2::float8) AS s, avg(n2::float8 * 1.1) AS a, sum(n2::float8 * grp) AS sg, percentile_cont(0.5) WITHIN GROUP (ORDER BY n2::float8) AS med
FROM t_num GROUP BY grp ORDER BY grp
