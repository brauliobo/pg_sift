-- mod and div of an integer column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 BETWEEN -500 AND 500 ORDER BY mod(i4, 7), div(i4, 50) DESC, id LIMIT 12
