-- ORDER BY a name column descending, NULLs first
-- shape: topn
-- expect: any
-- key: -
SELECT id, nm FROM t_chars ORDER BY nm DESC NULLS FIRST, id DESC LIMIT 30
