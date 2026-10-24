-- GREATEST of two text columns
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t, vc FROM t_text ORDER BY greatest(t, vc), id LIMIT 12
