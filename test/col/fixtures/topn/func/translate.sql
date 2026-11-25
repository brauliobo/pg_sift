-- translate of characters
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY translate(t, 'aeiou', 'AEIOU') DESC, id LIMIT 12
