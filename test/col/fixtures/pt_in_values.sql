-- IN a VALUES list and NOT EXISTS against one: a semi and an anti join of a list
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.deleted_at IS NULL AND o.tenant_id IN (VALUES (1), (2), (2), (3), (10), (NULL)) AND NOT EXISTS (SELECT 1 FROM (VALUES (3), (4)) v(id) WHERE v.id = o.tenant_id)
GROUP BY o.status ORDER BY o.status NULLS FIRST
