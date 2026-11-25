-- trap: the rows whose element is no number are dropped by the WHERE before the aggregate casts it: nothing is raised
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, avg((p ->> 'v')::float8) AS a
FROM t_vd, jsonb_array_elements(doc -> 'l') p WHERE grp NOT IN (2, 3, 4, 6)
GROUP BY grp ORDER BY grp
