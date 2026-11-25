-- ORDER BY a bytea column: the empty string, a zero byte, a prefix of another value and NULLs
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, b FROM t_misc ORDER BY b, id LIMIT 12 OFFSET 1
