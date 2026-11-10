-- trap: a list of two statuses does not imply the predicate (the rows of the second are in the heap only)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n FROM t_partial_hidden WHERE status IN ('open', 'held') GROUP BY grp ORDER BY grp
