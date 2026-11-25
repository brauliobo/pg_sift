-- trap: a disjunction with another condition does not imply the predicate
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n FROM t_partial_hidden WHERE status = 'open' OR grp = 2 GROUP BY grp ORDER BY grp
