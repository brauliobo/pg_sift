-- abs of the integers the WHERE keeps
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 > -2147483648 ORDER BY abs(i4) DESC, id LIMIT 12
