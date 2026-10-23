-- the first characters of a string
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY left(t, 3), id LIMIT 12
