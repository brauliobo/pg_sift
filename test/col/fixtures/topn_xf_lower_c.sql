-- lower() under the C collation: ASCII letters only, and the order of the bytes
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY lower(t COLLATE "C"), id LIMIT 12
