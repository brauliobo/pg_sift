-- a narrowing cast of the rows the WHERE keeps
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i8 FROM t_coal WHERE i8 BETWEEN -1000000 AND 1000000 ORDER BY i8::int, id LIMIT 12
