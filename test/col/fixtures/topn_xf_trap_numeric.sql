-- coalesce of a numeric column
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, total FROM orders ORDER BY coalesce(total, 0), id LIMIT 12
