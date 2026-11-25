-- coalesce of a numeric column keeps its scale
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, total FROM orders ORDER BY coalesce(total, 0), id LIMIT 12
