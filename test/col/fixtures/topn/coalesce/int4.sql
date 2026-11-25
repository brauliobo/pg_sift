-- coalesce of an int column, the constant the largest value of the type (it ties with the rows that hold it)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY coalesce(i4, 2147483647), id LIMIT 12
