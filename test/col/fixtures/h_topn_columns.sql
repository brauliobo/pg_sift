-- no text search: filters and the order from the columns
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, store_id, placed_at FROM orders WHERE status = 'shipped' AND store_id BETWEEN 50 AND 900 ORDER BY placed_at DESC, id DESC LIMIT 12
