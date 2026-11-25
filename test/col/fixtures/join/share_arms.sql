-- UNION ALL arms that join the same table with the same conditions and columns load it once
-- shape: join
-- expect: any
-- key: -
-- scenario: yes
SELECT 'a' AS arm, s.kind AS k, count(*) AS n FROM orders o JOIN tenants s ON s.id = o.tenant_id WHERE s.country_code = 'US' GROUP BY s.kind
UNION ALL
SELECT 'b', s.kind, count(*) FROM orders o JOIN tenants s ON s.id = o.tenant_id WHERE s.country_code = 'US' AND o.deleted_at IS NULL GROUP BY s.kind
ORDER BY 1, 2 NULLS FIRST
