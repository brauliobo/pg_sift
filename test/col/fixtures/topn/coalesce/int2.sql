-- coalesce of a smallint column and a constant between its limits, newest first
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, s2 FROM t_coal ORDER BY coalesce(s2, 0::smallint) DESC, id LIMIT 12
