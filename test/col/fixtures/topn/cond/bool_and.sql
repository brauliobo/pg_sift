-- a boolean expression as the key (false before true)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4, s2 FROM t_coal ORDER BY i4 > 0 AND s2 < 0, id LIMIT 12
