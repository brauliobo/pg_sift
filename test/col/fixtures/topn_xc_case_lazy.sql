-- a CASE keeps its lazy order: the division by zero is not evaluated for the row (i4 = 1) the WHEN leaves out
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 BETWEEN -1000000 AND 1000000 ORDER BY CASE WHEN i4 <> 1 THEN 1000 / (i4 - 1) ELSE 0 END, id LIMIT 12
