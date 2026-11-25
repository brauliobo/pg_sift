-- ORDER BY a bytea column descending with NULLs first
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, b FROM t_misc ORDER BY b DESC NULLS FIRST, id LIMIT 12
