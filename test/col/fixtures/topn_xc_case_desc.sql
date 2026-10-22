-- CASE descending with NULLS FIRST (a branch without ELSE gives NULL)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY CASE WHEN i4 > 100 THEN i4 END DESC NULLS FIRST, id LIMIT 12
