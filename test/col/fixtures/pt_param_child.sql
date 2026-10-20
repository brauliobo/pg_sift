-- the sub-select takes parameters (a LIMIT and a condition): the plan reads them like the normal plan, custom and generic
-- shape: join
-- expect: agg
-- key: -
-- prepare: (int, text)
-- args: 7, 'outlet'
SELECT s.kind, count(*) AS n, sum(o.total) AS revenue
FROM orders o JOIN (SELECT id, kind FROM tenants WHERE kind <> $2 ORDER BY id LIMIT $1) s ON o.tenant_id = s.id
WHERE o.deleted_at IS NULL
GROUP BY s.kind ORDER BY s.kind
