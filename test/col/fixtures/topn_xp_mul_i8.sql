-- bigint product: the values near the limits of the type
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i8 FROM t_coal WHERE i8 BETWEEN -1000000 AND 1000000 ORDER BY i8 * 3, id LIMIT 12
