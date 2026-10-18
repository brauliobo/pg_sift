-- trap: a division: its scale and rounding are numeric_div's (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, sum(n2 / 3) AS q FROM t_num GROUP BY grp ORDER BY grp
