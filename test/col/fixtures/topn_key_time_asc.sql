-- ORDER BY a time column ascending, NULLs first, with a filter
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_misc WHERE qty IS NOT NULL ORDER BY t NULLS FIRST, id LIMIT 11
