-- lower of a concatenation of two string columns
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t, vc FROM t_text ORDER BY lower(t || vc), id LIMIT 12
