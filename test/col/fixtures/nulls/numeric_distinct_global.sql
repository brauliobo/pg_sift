-- count(DISTINCT) of numeric values without a group, over all rows and over none
-- shape: notes-path-exists
-- expect: agg
-- key: -
SELECT count(DISTINCT n2) AS d2, count(DISTINCT n6) AS d6, count(DISTINCT n2 * n6) AS d_mul, count(DISTINCT n0) FILTER (WHERE id < 0) AS none FROM t_num
