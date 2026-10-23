-- a concatenation under the C collation
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t, vc FROM t_text ORDER BY (t || vc) COLLATE "C" DESC, id LIMIT 12
