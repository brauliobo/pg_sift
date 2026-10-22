-- ORDER BY a text column descending with NULLs first and a filter
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, t FROM t_text WHERE grp <> 2 ORDER BY t DESC NULLS FIRST, id LIMIT 15
