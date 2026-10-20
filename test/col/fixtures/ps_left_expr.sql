-- expressions over the NULL columns of the order table and over the dimension are PostgreSQL's: coalesce, a CASE, a concatenation
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind || '/' || coalesce(o.source, 'none') AS k, count(*) AS rows, sum(coalesce(o.id, 0) + s.id) AS mixed,
	   count(*) FILTER (WHERE o.id IS NULL) AS without_order, max(CASE WHEN o.id IS NULL THEN s.name END) AS last_store
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id AND o.id % 5 = 0
WHERE s.id > 300
GROUP BY 1 ORDER BY 1
