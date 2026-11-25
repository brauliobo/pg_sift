-- unary minus: the smallest integer overflows
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 > -2147483648 ORDER BY -i4, id LIMIT 12
