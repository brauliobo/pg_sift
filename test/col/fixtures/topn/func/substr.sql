-- a substring: many ties, multi-byte characters
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY substr(t, 2, 3) DESC, id LIMIT 12
