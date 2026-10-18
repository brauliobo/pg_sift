-- trap: a product of 54 digits that a floor brings back to 37: every step has to fit 128 bits, not only the result (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, max(floor(n6 * n6 * n6)) AS f FROM t_num GROUP BY grp ORDER BY grp
