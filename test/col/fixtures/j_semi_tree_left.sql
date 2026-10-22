-- the keys of a LEFT JOIN of two tables, behind a table of their own: a store without a row of the other table keeps its own key
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.deleted_at IS NULL
  AND o.tenant_id IN (SELECT t.id FROM tenants t WHERE t.id IN (SELECT coalesce(s.id, d.id) FROM tenants s LEFT JOIN tenants d ON d.id = s.id + 1 AND d.kind = 'popup' WHERE s.kind <> 'online'))
GROUP BY o.status ORDER BY o.status NULLS FIRST
