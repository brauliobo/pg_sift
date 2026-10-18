-- trap: numeric(18, 6) as float8 can lose digits (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, sum(n6::float8) AS f6 FROM t_num GROUP BY grp ORDER BY grp
