-- the sum overflows in a row that is not among the top rows: the error is raised like the plan's sort does (the same SQLSTATE)
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY i4 + 1, id LIMIT 12
