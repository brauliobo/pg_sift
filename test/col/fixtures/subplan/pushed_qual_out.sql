-- a condition pushed into the sub-select on a column the query reads as well: the plan outputs it
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, max(s.n) AS busiest, count(*) AS n
FROM orders o JOIN (SELECT tenant_id, count(*) AS n FROM orders GROUP BY tenant_id) s ON o.tenant_id = s.tenant_id
WHERE s.n > 20
GROUP BY o.status ORDER BY o.status
