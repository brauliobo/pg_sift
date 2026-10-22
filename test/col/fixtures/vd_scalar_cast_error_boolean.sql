-- trap: a boolean in a path declared float8 cast from its text raises the error of the cast, not a NULL
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- error: yes
-- key: -
SELECT count(*) AS n, sum((doc ->> 'k')::float8) AS s
FROM t_vd WHERE grp = 6
