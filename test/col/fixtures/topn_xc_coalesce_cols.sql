-- coalesce of two integer columns of different widths
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4, s2 FROM t_coal ORDER BY coalesce(i4, s2), id LIMIT 12
