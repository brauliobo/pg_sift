-- IN, EXISTS and NOT EXISTS against a tree of joined tables (an inner join; a LEFT JOIN whose result gives the keys): PostgreSQL's plan of the tree runs once and gives the keys
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue, count(DISTINCT o.tenant_id) AS tenants
FROM orders o
WHERE o.deleted_at IS NULL
  AND o.tenant_id IN (SELECT s.id FROM tenants s JOIN dim_country c ON c.country_code = s.country_code WHERE c.continent <> 'north')
  AND NOT EXISTS (SELECT 1 FROM tenants s2 JOIN dim_country c2 ON c2.country_code = s2.country_code WHERE s2.id = o.tenant_id AND s2.kind = 'outlet' AND c2.continent = 'east')
GROUP BY o.status ORDER BY o.status NULLS FIRST
