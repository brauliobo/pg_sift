-- trap: numeric compared with float8 is a float8 comparison (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, count(*) FILTER (WHERE n2 > 1.5::float8) AS gt FROM t_num GROUP BY grp ORDER BY grp
