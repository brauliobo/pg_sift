-- a narrowing cast raises (22003) for the values outside the type
-- shape: topn
-- expect: agg
-- error: yes
-- key: -
-- scenario: yes
SELECT id, i8 FROM t_coal ORDER BY i8::int, id LIMIT 12
