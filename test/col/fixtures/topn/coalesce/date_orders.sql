-- coalesce of a date and a constant date, over the table of orders
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, placed_at FROM orders ORDER BY coalesce(placed_at, DATE '2024-01-01') DESC, id LIMIT 12
