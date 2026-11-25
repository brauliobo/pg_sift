-- IN of constants
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp, i4 FROM t_coal ORDER BY i4 IN (1, 2, 3, 500), grp IN (1, 2) DESC, id LIMIT 12
