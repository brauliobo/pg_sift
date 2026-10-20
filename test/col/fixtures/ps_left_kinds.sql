-- the J07 sample: the groups of the dimension, count(o.id) against count(*) and the sum and average of a group whose orders are all deleted or none
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS orders, count(*) AS rows, sum(o.total) AS revenue, avg(o.discount) AS avg_discount, count(DISTINCT o.source) AS sources
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id AND o.deleted_at IS NULL AND o.status = 'refunded'
GROUP BY s.kind ORDER BY s.kind
