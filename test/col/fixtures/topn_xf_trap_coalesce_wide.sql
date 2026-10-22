-- coalesce of an int column and a bigint constant: a cast of the column
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY coalesce(i4, 5000000000), id LIMIT 12
