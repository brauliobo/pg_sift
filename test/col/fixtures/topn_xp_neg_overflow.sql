-- unary minus of the smallest integer raises
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY -i4, id LIMIT 12
