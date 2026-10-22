-- constant - column reverses the order of the column; the NULLs stay where the direction puts them
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, s2 FROM t_coal ORDER BY 1000 - s2 NULLS FIRST, id LIMIT 12
