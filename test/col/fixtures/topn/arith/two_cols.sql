-- two columns of different widths
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, s2, i4 FROM t_coal WHERE i4 BETWEEN -500 AND 500 ORDER BY s2 + i4, id LIMIT 12
