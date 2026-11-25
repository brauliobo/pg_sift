-- coalesce of a function of a string and another column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t, vc FROM t_text ORDER BY coalesce(left(t, 2), vc), id LIMIT 12
