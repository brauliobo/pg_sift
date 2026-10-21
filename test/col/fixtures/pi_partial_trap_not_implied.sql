-- trap: the conditions do not imply the predicate (a deleted row, a day before the bound would be missing from the index): the normal plan
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, sum(amount) AS total FROM t_partial WHERE day >= '2026-01-20' GROUP BY grp ORDER BY grp
