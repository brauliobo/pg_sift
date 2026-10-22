-- a FULL JOIN whose scanned side is a sub-select with a condition
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS n, count(*) AS tuples FROM (SELECT * FROM orders WHERE total > 100) o FULL JOIN tenants s ON s.id = o.tenant_id GROUP BY s.kind ORDER BY s.kind NULLS FIRST
