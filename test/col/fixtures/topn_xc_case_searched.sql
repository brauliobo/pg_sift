-- CASE WHEN: the NULLs become 0, a value in the middle of the others
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal WHERE i4 < 2147483647 ORDER BY CASE WHEN i4 IS NULL THEN 0 ELSE i4 END, id LIMIT 12
