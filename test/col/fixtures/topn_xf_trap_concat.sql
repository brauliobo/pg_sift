-- lower() of a concatenation
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY lower(t || vc), id LIMIT 12
