-- branches of different integer types are cast to the widest
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, s2, i8 FROM t_coal WHERE i8 BETWEEN -1000000 AND 1000000 ORDER BY CASE WHEN s2 > 0 THEN s2 ELSE i8 END DESC, id LIMIT 12
