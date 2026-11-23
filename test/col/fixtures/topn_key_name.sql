-- ORDER BY a name column: the C collation of the type
-- shape: topn
-- expect: any
-- key: -
SELECT id, nm FROM t_chars ORDER BY nm, id LIMIT 30
