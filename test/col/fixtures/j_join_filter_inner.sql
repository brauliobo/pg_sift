-- an inner join that returns a column of the table, selective on its own condition: the leaf is tested against the table's keys before the tuples are made
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, o.status, count(*) AS n, sum(o.total) AS revenue
FROM orders o JOIN tenants s ON s.id = o.tenant_id
WHERE o.deleted_at IS NULL AND s.country_code = 'DE'
GROUP BY s.kind, o.status ORDER BY s.kind, o.status NULLS FIRST
