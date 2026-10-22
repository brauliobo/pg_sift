-- NULLIF on a string column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY nullif(t, '') DESC NULLS LAST, id LIMIT 12
