-- a column first, an expression second
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp, i4 FROM t_coal WHERE i4 BETWEEN -500 AND 500 ORDER BY grp, i4 * 2 DESC, id LIMIT 12
