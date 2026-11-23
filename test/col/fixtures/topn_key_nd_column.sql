-- ORDER BY a text column declared under a nondeterministic collation, ties of case and accents broken by id
-- shape: topn
-- expect: agg
-- key: -
-- requires: SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'coldiff_nd')
SELECT id, nd FROM t_chars ORDER BY nd, id LIMIT 25 OFFSET 2
