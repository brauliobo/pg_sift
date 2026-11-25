-- coalesce of a date column (infinities among the values) and a constant date
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, d FROM t_coal ORDER BY coalesce(d, DATE '2024-01-15') DESC, id LIMIT 12
