-- a function of a function of one column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY lower(upper(t)), id LIMIT 12
