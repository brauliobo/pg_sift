-- count(DISTINCT) of numeric values without a group, over all rows and over none
-- shape: S6
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT count(DISTINCT n2) AS d2, count(DISTINCT n6) AS d6, count(DISTINCT n2 * n6) AS d_mul, count(DISTINCT n0) FILTER (WHERE id < 0) AS none FROM t_num
