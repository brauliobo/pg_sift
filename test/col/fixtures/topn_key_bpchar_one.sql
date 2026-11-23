-- ORDER BY a char(1): a space and accents
-- shape: topn
-- expect: agg
-- key: -
SELECT id, b1 FROM t_chars ORDER BY b1 DESC, id LIMIT 30
