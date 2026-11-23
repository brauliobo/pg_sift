-- ORDER BY a wide numeric cast to text
-- shape: topn
-- expect: agg
-- key: -
SELECT id, n38 FROM t_wide ORDER BY n38::text COLLATE "C", id LIMIT 12
