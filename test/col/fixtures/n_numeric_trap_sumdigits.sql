-- trap: a sum of a product of 30 digits: no bound on the rows keeps it in 128 bits, numeric_add sums it (generic aggregate)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, sum(n2 * n6) AS big FROM t_num GROUP BY grp ORDER BY grp
