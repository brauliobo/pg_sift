-- trap: a path declared float8 cast from its jsonb value (the index flags the values its float8 leaves cannot store, so the scan keeps the normal plan)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, sum((doc -> 'k')::float8) AS s
FROM t_vd WHERE grp IN (0, 11)
GROUP BY grp ORDER BY grp
