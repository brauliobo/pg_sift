-- the table is the fact table and, aggregated by store, the sub-select it joins (a child with a scan of its own)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue, max(s.n) AS busiest
FROM orders o JOIN (SELECT tenant_id, count(*) AS n FROM orders WHERE deleted_at IS NULL GROUP BY tenant_id HAVING count(*) > 5) s ON o.tenant_id = s.tenant_id
WHERE o.deleted_at IS NULL
GROUP BY o.status ORDER BY o.status NULLS FIRST
