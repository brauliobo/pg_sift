-- strpos of a constant in a string column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY strpos(t, 'a'), id LIMIT 12
