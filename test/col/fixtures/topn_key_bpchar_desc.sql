-- ORDER BY a char(6) descending, NULLs last
-- shape: topn
-- expect: agg
-- key: -
SELECT id, bp FROM t_chars ORDER BY bp DESC NULLS LAST, id LIMIT 30
