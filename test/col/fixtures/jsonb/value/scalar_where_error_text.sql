-- trap: a text that is no number in a path declared float8, cast by a condition that keeps the row, raises the error of the cast, not a NULL
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- error: yes
-- key: -
SELECT count(*) AS n
FROM t_vd WHERE (doc ->> 'k')::float8 > 1 AND grp = 2
