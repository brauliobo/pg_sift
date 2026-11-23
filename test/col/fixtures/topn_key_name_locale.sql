-- ORDER BY a name column under another collation
-- shape: topn
-- expect: any
-- key: -
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'und-x-icu')
SELECT id, nm FROM t_chars ORDER BY nm COLLATE "und-x-icu", id LIMIT 30
