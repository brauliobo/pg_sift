-- ORDER BY a text key under a nondeterministic collation: strings that are equal under it differ as bytes, and their order is the plan's, so it keeps the normal plan
-- shape: trap
-- expect: plain
-- key: -
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'coldiff_nd')
SELECT id, t FROM t_text ORDER BY t COLLATE "coldiff_nd", id LIMIT 12
