-- a LEFT JOIN to the ranked sub-select with the condition on the rank in its ON clause: the plan of the sub-select enforces it
-- shape: join
-- expect: any
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n, count(ranked.tenant_id) AS ranked_rows
FROM orders o JOIN tenants s ON s.id = o.tenant_id
LEFT JOIN (SELECT tenant_id, row_number() OVER (PARTITION BY kind ORDER BY n DESC, tenant_id) AS rn
           FROM (SELECT o2.tenant_id, s2.kind, count(*) AS n FROM orders o2 JOIN tenants s2 ON s2.id = o2.tenant_id WHERE o2.deleted_at IS NULL GROUP BY 1, 2) c) ranked
  ON ranked.tenant_id = o.tenant_id AND ranked.rn <= 2
GROUP BY 1
ORDER BY 1
