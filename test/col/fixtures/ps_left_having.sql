-- the stores with no order: HAVING over the groups of the pseudo rows
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.id, count(o.id) AS orders
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id AND o.deleted_at IS NULL
GROUP BY s.id HAVING count(o.id) = 0 ORDER BY s.id
