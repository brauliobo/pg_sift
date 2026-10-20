-- a join to the first stores in an order: LEFT keeps the orders of the others with a NULL kind
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n, sum(o.total) AS revenue
FROM orders o LEFT JOIN (SELECT id, kind FROM tenants ORDER BY name DESC, id LIMIT 12) s ON o.tenant_id = s.id
WHERE o.deleted_at IS NULL
GROUP BY s.kind ORDER BY s.kind NULLS FIRST
