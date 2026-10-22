-- date plus days; infinite dates stay infinite
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, d FROM t_coal ORDER BY d + 30 NULLS FIRST, id LIMIT 12
