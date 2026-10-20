-- an ON clause no order meets: every store is a row of NULLs
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS orders, count(*) AS rows, sum(o.total) AS revenue
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id AND o.id < 0
GROUP BY s.kind ORDER BY s.kind
