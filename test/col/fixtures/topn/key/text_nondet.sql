-- ORDER BY a text key under a nondeterministic collation: strings that are equal under it differ as bytes and tie, then the next key orders them (as the heap plan does)
-- shape: topn
-- expect: agg
-- key: -
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'coldiff_nd')
SELECT id, t FROM t_text ORDER BY t COLLATE "coldiff_nd", id LIMIT 12
