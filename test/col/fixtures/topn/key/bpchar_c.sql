-- ORDER BY a char(6) under the C collation
-- shape: topn
-- expect: agg
-- key: -
SELECT id, bp FROM t_chars ORDER BY bp COLLATE "C", id LIMIT 30
