-- GREATEST ignores NULLs
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4, s2 FROM t_coal ORDER BY greatest(i4, s2), id LIMIT 12
