-- trap: the rows whose text is no number are dropped by a cheaper condition before the cast of the condition runs: nothing is raised
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n
FROM t_vd WHERE (doc ->> 'k')::float8 >= 0 AND grp NOT IN (2, 3, 4, 6)
GROUP BY grp ORDER BY grp
