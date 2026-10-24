-- a semi join on a text column has no integer keys for the scan
-- shape: topn
-- expect: plain
-- key: -
-- scenario: yes
SELECT id, store_id, total FROM orders WHERE status IN (SELECT kind FROM stores) ORDER BY total DESC, id LIMIT 10
