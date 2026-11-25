-- coalesce of a timestamp column (mostly NULL) and a constant timestamp, over the table of orders
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, deleted_at FROM orders ORDER BY coalesce(deleted_at, TIMESTAMP '1999-01-01') DESC, id DESC LIMIT 12
