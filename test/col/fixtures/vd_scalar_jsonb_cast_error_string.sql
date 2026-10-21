-- trap: a string in a path declared float8 cast from its jsonb value raises the error of the cast, not a NULL
-- shape: trap
-- expect: plain
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- error: yes
-- key: -
SELECT count(*) AS n, sum((doc -> 'k')::float8) AS s
FROM t_vd WHERE grp = 2
