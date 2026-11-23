-- ORDER BY a nondeterministic text column descending, NULLs first
-- shape: topn
-- expect: agg
-- key: -
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'coldiff_nd')
SELECT id, nd FROM t_chars ORDER BY nd DESC NULLS FIRST, id DESC LIMIT 25
