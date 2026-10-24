-- CASE on a comparison of a string column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY CASE WHEN t = 'a' THEN 0 WHEN t < 'B' THEN 1 ELSE 2 END, id LIMIT 12
