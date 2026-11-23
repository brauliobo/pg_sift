-- ORDER BY a nondeterministic text column then a second key (the ties the collation makes are the second key's)
-- shape: topn
-- expect: agg
-- key: -
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'coldiff_nd')
SELECT id, nd, grp FROM t_chars ORDER BY nd, grp DESC, id LIMIT 30
