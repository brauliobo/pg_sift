-- a condition of the ON clause on the preserved dimension does not filter it: a store that fails it stays, without a partner
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.country_code, count(o.id) AS orders, count(*) AS rows, sum(o.total) AS revenue
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id AND s.kind <> 'outlet' AND o.total > 100
GROUP BY s.country_code ORDER BY s.country_code
