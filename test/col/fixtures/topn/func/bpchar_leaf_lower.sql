-- ORDER BY lower of a char(n) leaf
-- shape: topn
-- expect: agg
-- key: -
SELECT id, bp FROM t_chars ORDER BY lower(bp), id LIMIT 12
