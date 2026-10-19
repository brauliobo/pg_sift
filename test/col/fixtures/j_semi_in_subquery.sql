-- IN (SELECT ...) with a condition on the small table, and NOT EXISTS with two keys
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.deleted_at IS NULL AND o.tenant_id IN (SELECT s.id FROM tenants s WHERE s.kind = 'outlet')
  AND NOT EXISTS (SELECT 1 FROM tenants s2 WHERE s2.id = o.tenant_id AND s2.country_code = o.country_code)
GROUP BY o.status ORDER BY o.status NULLS FIRST
