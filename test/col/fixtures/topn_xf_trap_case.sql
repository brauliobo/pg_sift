-- CASE
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY CASE WHEN i4 IS NULL THEN 0 ELSE i4 END, id LIMIT 12
