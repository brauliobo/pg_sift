-- a CASE with a function of a string in its branch
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY CASE WHEN t IS NULL THEN '' ELSE left(t, 3) END, id LIMIT 12
