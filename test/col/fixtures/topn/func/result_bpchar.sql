-- ORDER BY a char(n) result: the column cast to char(3), trailing spaces are not significant
-- shape: topn
-- expect: agg
-- key: -
SELECT id, bp FROM t_chars ORDER BY bp::char(3), id LIMIT 12
