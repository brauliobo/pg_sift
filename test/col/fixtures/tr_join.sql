-- ctid: a range of the orders next to a lookup join (the dimension's own columns are tested as before)
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
SELECT s.kind, count(*) AS n, sum(o.total) AS revenue
FROM orders o JOIN tenants s ON s.id = o.tenant_id AND s.kind <> 'outlet'
WHERE o.ctid >= '(60,1)' AND o.ctid < '(500,1)' AND s.country_code IN ('US', 'GB', 'DE')
GROUP BY s.kind ORDER BY s.kind
