-- ORDER BY a char(6): trailing spaces are not significant, the empty string, NULLs
-- shape: topn
-- expect: agg
-- key: -
SELECT id, bp FROM t_chars ORDER BY bp, id LIMIT 30 OFFSET 1
