-- a LEFT JOIN to a sub-select that returns no row: every order once, with a NULL kind
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n, sum(o.total) AS revenue
FROM orders o LEFT JOIN (SELECT id, kind FROM tenants WHERE id < 0 LIMIT 5) s ON o.tenant_id = s.id
WHERE o.deleted_at IS NULL
GROUP BY s.kind ORDER BY s.kind NULLS FIRST
