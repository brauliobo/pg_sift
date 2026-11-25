-- coalesce of three columns and a constant
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, s2, i4, i8 FROM t_coal WHERE i8 BETWEEN -1000000 AND 1000000 ORDER BY coalesce(s2, i4, i8, 0) DESC, id LIMIT 12
