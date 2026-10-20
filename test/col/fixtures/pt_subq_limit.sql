-- a join to a sub-select with LIMIT (the sample of the coverage table)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n, sum(o.total) AS revenue
FROM orders o JOIN (SELECT id, kind FROM tenants LIMIT 5) s ON o.tenant_id = s.id
WHERE o.deleted_at IS NULL
GROUP BY s.kind ORDER BY s.kind
