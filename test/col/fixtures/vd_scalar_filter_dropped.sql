-- trap: the rows whose text is no number are dropped by the WHERE before the FILTER casts it: nothing is raised
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, count(*) FILTER (WHERE (doc ->> 'k')::float8 >= 0) AS ok
FROM t_vd WHERE grp NOT IN (2, 3, 4, 6)
GROUP BY grp ORDER BY grp
