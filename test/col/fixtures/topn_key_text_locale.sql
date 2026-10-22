-- ORDER BY a text column under a collation of the C library other than C (strcoll orders the accents and the case)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'en_US.utf8')
SELECT id, t FROM t_text ORDER BY t COLLATE "en_US.utf8", id LIMIT 14
