-- ORDER BY a money column ascending, NULLs first
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, m FROM t_wide ORDER BY m NULLS FIRST, id LIMIT 9
