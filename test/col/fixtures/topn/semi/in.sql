-- a Top-N of the rows whose key is in a sub-select: PostgreSQL pulls it up into a semi join, whose keys are a conjunct of the scan
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, tenant_id, total FROM orders WHERE tenant_id IN (SELECT id FROM tenants WHERE kind = 'outlet') ORDER BY total DESC, id LIMIT 15
