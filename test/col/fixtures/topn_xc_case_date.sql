-- CASE with date branches
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, d FROM t_coal ORDER BY CASE WHEN d IS NULL THEN DATE '2024-02-01' ELSE d END DESC, id LIMIT 12
