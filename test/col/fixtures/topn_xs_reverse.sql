-- reverse of a string
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY reverse(t), id LIMIT 12
