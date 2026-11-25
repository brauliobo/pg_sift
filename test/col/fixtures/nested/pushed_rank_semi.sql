-- the stores that rank in the top three of their kind by orders, as an IN: the ranking window's run condition `rn <= 3` is the plan's own, the scan does not test it again
-- shape: join
-- expect: any
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n, sum(o.id) AS ids
FROM orders o JOIN tenants s ON s.id = o.tenant_id
WHERE o.tenant_id IN (SELECT tenant_id FROM (
    SELECT tenant_id, row_number() OVER (PARTITION BY kind ORDER BY n DESC, tenant_id) AS rn
    FROM (SELECT o2.tenant_id, s2.kind, count(*) AS n FROM orders o2 JOIN tenants s2 ON s2.id = o2.tenant_id WHERE o2.deleted_at IS NULL GROUP BY 1, 2) c) ranked
  WHERE rn <= 3)
GROUP BY 1
ORDER BY 1
