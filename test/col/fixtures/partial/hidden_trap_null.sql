-- trap: a status that is NULL is not the predicate (a comparison with NULL keeps no row): the normal plan, no row
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n FROM t_partial_hidden WHERE status IS NULL GROUP BY grp ORDER BY grp
