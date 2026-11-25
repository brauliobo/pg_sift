-- integer to numeric
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY i4::numeric DESC, id LIMIT 12
