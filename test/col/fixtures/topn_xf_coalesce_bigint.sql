-- coalesce of a bigint column and a constant at the limits of the type
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i8 FROM t_coal ORDER BY coalesce(i8, 9223372036854775807) DESC, id LIMIT 12
