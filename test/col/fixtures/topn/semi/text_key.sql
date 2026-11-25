-- a semi join on a text column: the keys of the joined table are the exact condition of the Top-N scan (text keys, 4.1)
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, total FROM orders WHERE status IN (SELECT kind FROM tenants) ORDER BY total DESC, id LIMIT 10
