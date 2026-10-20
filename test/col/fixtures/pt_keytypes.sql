-- the keys of the sub-selects are of other types than the leaves: bigint against an integer leaf, varchar against a text leaf
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, c.continent, count(*) AS n, sum(o.total) AS revenue
FROM orders o
JOIN (SELECT id::bigint AS id, kind FROM tenants WHERE id < 200 OFFSET 0) s ON o.tenant_id = s.id
LEFT JOIN (SELECT country_code::varchar AS cc, 'x' || country_code AS continent FROM tenants GROUP BY country_code) c ON o.country_code = c.cc
WHERE o.deleted_at IS NULL
GROUP BY s.kind, c.continent ORDER BY s.kind, c.continent NULLS FIRST
