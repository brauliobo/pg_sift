-- abs of an integer column: the smallest integer raises (22003)
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY abs(i4), id LIMIT 12
