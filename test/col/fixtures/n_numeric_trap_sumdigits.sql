-- trap: a sum of a product of 30 digits: no bound on the number of rows keeps it in 128 bits (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, sum(n2 * n6) AS big FROM t_num GROUP BY grp ORDER BY grp
