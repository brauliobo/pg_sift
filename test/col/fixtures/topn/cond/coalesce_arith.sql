-- arithmetic of a coalesce of two columns
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4, s2 FROM t_coal WHERE i4 < 1000000 ORDER BY coalesce(i4, s2, 0) + 1, id LIMIT 12
