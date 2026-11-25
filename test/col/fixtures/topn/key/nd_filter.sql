-- ORDER BY a nondeterministic text column with a filter
-- shape: topn
-- expect: agg
-- key: -
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'coldiff_nd')
SELECT id, nd FROM t_chars WHERE grp <> 2 ORDER BY nd, id LIMIT 20
