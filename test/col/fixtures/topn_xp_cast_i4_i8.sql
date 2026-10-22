-- a widening cast keeps the order
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, i4 FROM t_coal ORDER BY i4::bigint, id LIMIT 12
