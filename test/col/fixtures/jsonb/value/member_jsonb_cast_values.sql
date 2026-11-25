-- trap: the member of an object leaf cast from its jsonb value: only numbers cast (the index flags the values its float8 leaves cannot store, so the scan keeps the normal plan)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, sum((doc -> 'm' -> 'a')::float8) AS s, max((doc -> 'm' -> 'a')::float8) AS hi
FROM t_vd WHERE grp IN (0, 11)
GROUP BY grp ORDER BY grp
