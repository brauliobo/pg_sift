-- the filter keeps no row
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 BETWEEN 600 AND 700 ORDER BY i4 * 2, id LIMIT 12
