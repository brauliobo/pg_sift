-- bigint product that overflows in the rows with the limits of the type
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- scenario: yes
SELECT id, i8 FROM t_coal ORDER BY i8 * 3 DESC, id LIMIT 12
