-- an inner join to a sub-select that returns no row: no group
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n, sum(o.total) AS revenue
FROM orders o JOIN (SELECT id, kind FROM tenants WHERE id < 0 LIMIT 5) s ON o.tenant_id = s.id
GROUP BY s.kind ORDER BY s.kind
