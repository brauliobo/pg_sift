-- IS TRUE, IS NOT TRUE and IS DISTINCT FROM
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4, s2 FROM t_coal ORDER BY (i4 > 0) IS NOT TRUE, i4 IS DISTINCT FROM s2 DESC, id LIMIT 12
