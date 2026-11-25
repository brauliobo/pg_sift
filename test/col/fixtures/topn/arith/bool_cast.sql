-- a comparison as the key (false before true)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 BETWEEN -500 AND 500 ORDER BY i4 > 0, id LIMIT 12
