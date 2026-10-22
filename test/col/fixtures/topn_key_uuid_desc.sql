-- ORDER BY a uuid column descending with NULLs first
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, u FROM t_misc ORDER BY u DESC NULLS FIRST, id LIMIT 12
