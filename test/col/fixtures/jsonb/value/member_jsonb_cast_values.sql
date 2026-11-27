-- trap: the member of an object leaf cast from its jsonb value: only numbers cast (the scan reads a value its float8 leaf cannot store from the exception of its element, and converts it as PostgreSQL does) [PostgreSQL 18 on: before it the cast of a jsonb member is not read]
-- shape: trap
-- expect: agg
-- requires: SELECT current_setting('server_version_num')::int >= 180000
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, sum((doc -> 'm' -> 'a')::float8) AS s, max((doc -> 'm' -> 'a')::float8) AS hi
FROM t_vd WHERE grp IN (0, 11)
GROUP BY grp ORDER BY grp
