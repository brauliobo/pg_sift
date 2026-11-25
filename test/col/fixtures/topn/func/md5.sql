-- a hash sorts as a string of hex digits
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY md5(t), id LIMIT 12
