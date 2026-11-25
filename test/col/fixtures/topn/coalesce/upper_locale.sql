-- upper() under a collation of the C library, then the order under another one
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'en_US.utf8')
SELECT id, t FROM t_text ORDER BY upper(t COLLATE "en_US.utf8") DESC, id LIMIT 12
