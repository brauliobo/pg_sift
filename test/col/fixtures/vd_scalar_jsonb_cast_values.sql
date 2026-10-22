-- trap: a path declared float8 cast from its jsonb value (the index flags the values its float8 leaves cannot store: the scan reads those rows again from the heap) [PostgreSQL 18 on: before it the scan is refused, slug error_parity]
-- shape: trap
-- expect: agg
-- requires: SELECT current_setting('server_version_num')::int >= 180000
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: 1
SELECT grp, count(*) AS n, sum((doc -> 'k')::float8) AS s
FROM t_vd WHERE grp IN (0, 11)
GROUP BY grp ORDER BY grp
