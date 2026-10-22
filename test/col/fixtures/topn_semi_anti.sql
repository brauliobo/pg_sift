-- NOT EXISTS is an anti join: the normal plan stays
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, store_id, total FROM orders o WHERE NOT EXISTS (SELECT 1 FROM stores s WHERE s.id = o.store_id AND s.kind = 'outlet') ORDER BY total DESC, id LIMIT 10
