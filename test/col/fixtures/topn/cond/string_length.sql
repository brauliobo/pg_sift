-- length of a string column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY length(t) DESC NULLS LAST, id LIMIT 12
