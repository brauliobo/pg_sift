-- repeat of the start of a string
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY repeat(left(t, 2), 3), id LIMIT 12
