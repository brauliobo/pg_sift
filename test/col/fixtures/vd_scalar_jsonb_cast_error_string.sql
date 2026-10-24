-- trap: a string in a path declared float8 cast from its jsonb value raises the error of the cast, not a NULL [PostgreSQL 18 on: before it the scan is refused, slug error_parity]
-- shape: trap
-- expect: agg
-- requires: SELECT current_setting('server_version_num')::int >= 180000
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- error: yes
-- key: -
SELECT count(*) AS n, sum((doc -> 'k')::float8) AS s
FROM t_vd WHERE grp = 2
