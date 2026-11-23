-- ORDER BY a cidr column then an inet column
-- shape: topn
-- expect: agg
-- key: -
SELECT id, cd, ip FROM t_net ORDER BY cd, ip DESC, id LIMIT 12 OFFSET 1
