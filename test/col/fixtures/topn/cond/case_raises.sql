-- the division by zero of a branch that is taken raises the same SQLSTATE as the sort of the plan
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 BETWEEN -1000000 AND 1000000 ORDER BY CASE WHEN i4 >= 1 THEN 1000 / (i4 - 1) ELSE 0 END, id LIMIT 12
