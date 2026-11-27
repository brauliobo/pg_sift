-- trap: the member of an object leaf with free keys cast from its text: numbers, numeric strings, a JSON null, NaN, Infinity and -0 as PostgreSQL reads them (the scan reads a value its float8 leaf cannot store from the exception of its element, and converts it as PostgreSQL does)
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, count((doc -> 'm' ->> 'a')::float8) AS nv, sum((doc -> 'm' ->> 'a')::float8) AS s, min((doc -> 'm' ->> 'a')::float8) AS lo, max((doc -> 'm' ->> 'a')::float8) AS hi
FROM t_vd WHERE grp IN (0, 1, 5, 7, 8, 9, 10, 11)
GROUP BY grp ORDER BY grp
