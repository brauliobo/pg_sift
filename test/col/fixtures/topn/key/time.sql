-- ORDER BY a time column: 24:00:00, 00:00:00, microseconds and NULLs
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_misc ORDER BY t DESC NULLS LAST, id LIMIT 8 OFFSET 1
