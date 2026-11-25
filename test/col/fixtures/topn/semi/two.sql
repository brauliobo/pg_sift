-- two sub-selects, one with duplicate keys (a semi join matches a row once)
-- shape: topn
-- expect: agg
-- guc: enable_seqscan = off
-- guc: enable_indexscan = off
-- guc: enable_bitmapscan = off
-- key: -
-- scenario: yes
SELECT id, tenant_id, total FROM orders WHERE tenant_id IN (SELECT id FROM tenants WHERE id % 3 = 0) AND tenant_id IN (SELECT tenant_id FROM orders_odd WHERE total > 300) ORDER BY total, id LIMIT 12
