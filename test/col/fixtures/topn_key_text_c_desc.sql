-- ORDER BY a text column descending under the C collation
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text ORDER BY t COLLATE "C" DESC NULLS LAST, id DESC LIMIT 12
