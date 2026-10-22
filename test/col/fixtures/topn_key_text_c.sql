-- ORDER BY a text column under the C collation: byte order, uppercase before lowercase, accents last
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY t COLLATE "C", id LIMIT 12
