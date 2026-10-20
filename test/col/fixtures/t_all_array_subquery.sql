-- <> ALL of an ARRAY sub-select: an initplan (a parameter of the query) tested as a lookup table over the leaf
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND o.tenant_id <> ALL (ARRAY(SELECT s.id FROM tenants s WHERE s.id < 380))
GROUP BY o.status ORDER BY o.status NULLS FIRST
