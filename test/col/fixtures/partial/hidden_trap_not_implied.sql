-- trap: no condition on the status (the rows of the other statuses are in the heap only): the normal plan
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, sum(amount) AS total FROM t_partial_hidden WHERE grp < 5 GROUP BY grp ORDER BY grp
