-- CASE with text branches: the string order of the collation
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY CASE WHEN t IS NULL THEN '' WHEN length(t) > 3 THEN 'long' ELSE t END, id LIMIT 12
