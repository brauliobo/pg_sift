-- replace of a constant
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY replace(t, 'a', 'z'), id LIMIT 12
