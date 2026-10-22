-- trap: a condition that casts the jsonb value of a path declared float8 (the index flags the values its float8 leaves cannot store: the scan reads those rows again from the heap)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, sum((doc -> 'k')::float8) AS s
FROM t_vd WHERE (doc -> 'k')::float8 >= 0 AND grp IN (0, 11)
GROUP BY grp ORDER BY grp
