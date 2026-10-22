-- coalesce of an integer column and a bigint constant (the column is cast)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY coalesce(i4, 5000000000), id LIMIT 12
