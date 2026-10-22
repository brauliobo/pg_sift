-- OR with NULLs (three-valued logic)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4, s2 FROM t_coal ORDER BY i4 IS NULL OR s2 IS NULL, i4 > 0 OR s2 > 0 NULLS FIRST, id LIMIT 12
