-- trap: a path declared float8 cast from its text: numeric strings, a JSON null, NaN, Infinity and -0 as PostgreSQL reads them (the index flags the values its float8 leaves cannot store, so the scan keeps the normal plan)
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, count((doc ->> 'k')::float8) AS nv, sum((doc ->> 'k')::float8) AS s, min((doc ->> 'k')::float8) AS lo, max((doc ->> 'k')::float8) AS hi
FROM t_vd WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11)
GROUP BY grp ORDER BY grp
