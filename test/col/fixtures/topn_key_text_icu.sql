-- ORDER BY a text column under an ICU collation
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'und-x-icu')
SELECT id, t FROM t_text ORDER BY t COLLATE "und-x-icu" DESC, id LIMIT 14
