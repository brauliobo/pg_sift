-- an array result: refused
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY ARRAY[grp, 1], id LIMIT 12
