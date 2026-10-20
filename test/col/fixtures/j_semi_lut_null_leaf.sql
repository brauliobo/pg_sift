-- a nullable integer leaf against a table's keys: a NULL customer matches nothing (dropped by IN, kept by NOT EXISTS)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, count(o.customer_id) AS with_customer,
       count(*) FILTER (WHERE o.tenant_id > 200) AS far
FROM orders o
WHERE o.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM tenants s WHERE s.id = o.customer_id AND s.kind = 'online')
GROUP BY o.status
UNION ALL
SELECT 'in', count(*), count(o.customer_id), count(*) FILTER (WHERE o.tenant_id > 200)
FROM orders o WHERE o.deleted_at IS NULL AND o.customer_id IN (SELECT s.id FROM tenants s WHERE s.id % 3 = 0)
ORDER BY 1 NULLS FIRST
