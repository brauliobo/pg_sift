-- trap: the cast of a member of an object leaf in a conjunct and as a group key (the scan reads a value its float8 leaf cannot store from the exception of its element, and converts it as PostgreSQL does)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT (doc -> 'm' ->> 'a')::float8 AS a, count(*) AS n
FROM t_vd WHERE grp IN (0, 1, 7, 8, 10, 11) AND (doc -> 'm' ->> 'a')::float8 > 1
GROUP BY 1 ORDER BY 1
