-- the rank is read by the query too: its column stays in the output of the plan of the sub-select
-- shape: join
-- expect: any
-- key: -
-- scenario: yes
SELECT ranked.rn, count(*) AS n
FROM orders o JOIN (SELECT tenant_id, row_number() OVER (PARTITION BY kind ORDER BY n DESC, tenant_id) AS rn
           FROM (SELECT o2.tenant_id, s2.kind, count(*) AS n FROM orders o2 JOIN tenants s2 ON s2.id = o2.tenant_id WHERE o2.deleted_at IS NULL GROUP BY 1, 2) c) ranked
  ON ranked.tenant_id = o.tenant_id
WHERE ranked.rn <= 3
GROUP BY 1
ORDER BY 1
