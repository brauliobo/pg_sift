-- a WHERE on the preserved dimension filters before the join: the stores that pass keep their rows with and without orders
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS orders, count(*) AS rows, max(o.total) AS biggest
FROM tenants s LEFT JOIN orders o ON o.tenant_id = s.id
WHERE s.kind IN ('flagship', 'popup') AND s.country_code <> 'GB'
GROUP BY s.kind ORDER BY s.kind
