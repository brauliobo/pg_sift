-- the product of two columns, descending
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, s2, grp FROM t_coal WHERE s2 BETWEEN -100 AND 100 ORDER BY s2 * grp DESC NULLS FIRST, id LIMIT 12
