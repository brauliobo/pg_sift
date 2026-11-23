-- ORDER BY a cidr column descending, NULLs first
-- shape: topn
-- expect: agg
-- key: -
SELECT id, cd FROM t_net ORDER BY cd DESC NULLS FIRST, id LIMIT 12 OFFSET 1
