-- ORDER BY an inet column descending with NULLs first
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, ip FROM t_net ORDER BY ip DESC NULLS FIRST, id LIMIT 12
