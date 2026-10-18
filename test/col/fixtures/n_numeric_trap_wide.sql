-- trap: numeric(30, 10) has no scaled integer form: the column is not in the index (the normal plan, same rows)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, sum(n) AS s, count(*) FILTER (WHERE n > 1) AS big FROM t_int GROUP BY grp ORDER BY grp
