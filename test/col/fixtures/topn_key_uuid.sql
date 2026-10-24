-- ORDER BY a uuid column: the all-zero and all-one values, repeated values and NULLs
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, u FROM t_misc ORDER BY u, id LIMIT 12 OFFSET 1
