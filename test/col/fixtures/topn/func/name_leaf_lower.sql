-- ORDER BY lower of a name leaf (a sift_table keeps a name as an opaque value: its results are the plan's, the plan may be the normal one)
-- shape: topn
-- expect: any
-- key: -
SELECT id, nm FROM t_chars ORDER BY lower(nm::text) DESC, id LIMIT 12
