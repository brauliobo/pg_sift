-- trap: a text that is no number in a path declared float8, cast by the FILTER of an aggregate, raises the error of the cast for the row that reaches it
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- error: yes
-- key: -
SELECT count(*) FILTER (WHERE (doc ->> 'k')::float8 > 1) AS n
FROM t_vd WHERE grp = 2
