-- an expression of an expression
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY lower(upper(t)), id LIMIT 12
