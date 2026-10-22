-- NULLIF turns a value into NULL
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp FROM t_coal ORDER BY nullif(grp, 2) NULLS FIRST, id LIMIT 12
