-- conditions on the columns of an inner join in the ON clause and in the WHERE, and the same ON clause in a LEFT JOIN with a WHERE that keeps the NULL row
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, o.status, count(*) AS n, sum(o.total) AS revenue
FROM orders o JOIN tenants s ON s.id = o.tenant_id AND s.kind <> 'outlet' AND s.name <> 'Store 7'
WHERE o.deleted_at IS NULL AND s.country_code IN ('US', 'GB', 'DE') AND (o.country_code = s.country_code OR o.source = 'web')
GROUP BY s.kind, o.status ORDER BY s.kind, o.status NULLS FIRST
