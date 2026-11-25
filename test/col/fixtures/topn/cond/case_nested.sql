-- a CASE inside a CASE and arithmetic of the branches
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4, s2 FROM t_coal WHERE i4 BETWEEN -500 AND 500 ORDER BY CASE WHEN i4 IS NULL THEN CASE WHEN s2 IS NULL THEN 0 ELSE s2 END ELSE i4 * 2 END, id LIMIT 12
