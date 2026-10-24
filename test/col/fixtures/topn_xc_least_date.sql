-- LEAST of a date column and a constant
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, d FROM t_coal ORDER BY least(d, DATE '2024-02-01') DESC, id LIMIT 12
