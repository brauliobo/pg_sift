-- trap: the column of the predicate is a group key, which the index does not hold
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT status, count(*) AS n FROM t_partial_hidden WHERE status = 'open' GROUP BY status ORDER BY status
