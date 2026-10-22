-- a selective filter with an expression key
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 BETWEEN 100 AND 130 AND grp = 2 ORDER BY i4 * 2, id LIMIT 12
